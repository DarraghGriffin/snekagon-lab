// Win test that looks for cheap proofs first: does player A (the attacker) win?
//
// The plain alpha-beta win test commits to the first attacker move that works, although other
// winning moves often have much cheaper proofs: a move's proof costs the sum over the defender's
// replies, and the typical cost of a reply differs ~10x between moves in ways the evaluation does
// not see. So at an expensive attacker node the best moves by the evaluation are screened first:
// a sample of each move's replies (spread over the evaluation order) gets alpha-beta probes with a
// small node budget, and replies x the share of the sample that exceeds the budget estimates the
// move's proof number (how many hard replies its proof has). The samples grow in rounds while the
// field of moves is halved (successive halving); a sampled reply that refutes a move drops it at
// once. Then the moves are proved in the order of the estimate.
//
// Every answer comes from exact alpha-beta win tests (one Solver, one table for all probes), so
// budgets and samples only change which proof is looked for first, never the answer.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "notation.hpp"
#include "solver.hpp"

class ScreenedWinTest {
  public:
    Solver solver;               // runs every probe and proof
    uint64_t probes = 0;         // alpha-beta probes of the last test
    uint64_t screens = 0;        // attacker nodes screened by the last test
    uint64_t valueNodes = 0;     // nodes of the last value(): both of its tests

    // Settings (environment variables override them for experiments).
    int screenFromPieces = envInt("PNS_SCREEN_REM", 10);  // screen attacker nodes with at least this many pieces left
    int screenedMoves = envInt("PNS_CANDIDATES", 8);       // moves screened per node
    uint64_t hardReplyNodes = envInt("PNS_HARD", 1000);    // a reply with 8 pieces left is hard above this many nodes
    uint64_t sampleSeed = envInt("PNS_SEED", 0);           // other samples (to measure their spread)
    bool trace = envInt("PNS_TRACE", 0) != 0;              // print every screen

    // The probes order the moves with the killers behind the evaluation: a move that made cutoffs
    // in one subtree usually wins in the next as well, but often with a much bigger proof than
    // other winning moves, and the win test needs about half the nodes without that preference
    // (SOLVER_KILLERS_OVER_EVAL=1 restores it).
    explicit ScreenedWinTest(int log2Entries = 22) : solver(log2Entries) {
        solver.killersOverEval = envInt("SOLVER_KILLERS_OVER_EVAL", 0) != 0;
    }

    void clear() { solver.clear(); }

    // Nodes of the last test: the solver's plus the positions evaluated for move ordering.
    uint64_t nodes() const { return solver.nodes + orderingNodes; }

    // Does `attacker` win from `root`? 1 yes, 0 no, Solver::UNKNOWN out of time (seconds = 0: no limit).
    int attackerWins(const Position& root, int attacker, double seconds = 0) {
        this->attacker = attacker;
        solver.nodes = 0;
        probes = screens = orderingNodes = 0;
        aborted = false;
        solver.setTimeLimit(seconds);
        int result = root.sideToMove == attacker ? someMoveWins(root) : everyReplyLoses(root);
        return aborted ? Solver::UNKNOWN : result;
    }

    // Exact value for the side to move (Solver::UNKNOWN if out of time): does it win, and if not,
    // does the opponent? valueNodes counts the nodes of both questions.
    int value(const Position& pos, double seconds = 0) {
        auto start = std::chrono::steady_clock::now();
        int wins = attackerWins(pos, pos.sideToMove, seconds);
        valueNodes = nodes();
        if (wins != 0) return wins;
        double timeLeft = seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (seconds > 0 && timeLeft <= 0) return Solver::UNKNOWN;
        int loses = attackerWins(pos, pos.sideToMove ^ 1, seconds > 0 ? timeLeft : 0);
        valueNodes += nodes();
        return loses == 1 ? -1 : loses == 0 ? 0 : Solver::UNKNOWN;
    }

  private:
    static constexpr int ROUNDS = 3;
    static constexpr int SAMPLES_PER_ROUND[ROUNDS] = {8, 8, 16};  // replies probed per move and round
    static constexpr int PROBE_RAN_OUT = -1;                        // probe(): the node budget ran out

    int attacker = 0;
    bool aborted = false;         // out of time
    uint64_t orderingNodes = 0;   // positions evaluated to order moves

    static int piecesLeft(const Position& pos) { return pieceCount(pos.piecesLeft[RED]) + pieceCount(pos.piecesLeft[BLUE]); }

    // Node budgets follow the typical cost of a node: about 10x per two more pieces left.
    static uint64_t budgetFor(uint64_t nodesAt8Pieces, int piecesLeft) {
        uint64_t budget = nodesAt8Pieces;
        for (; piecesLeft < 8; piecesLeft += 2) budget /= 10;
        for (; piecesLeft > 9; piecesLeft -= 2) budget *= 10;
        return std::max<uint64_t>(budget, 16);
    }

    // Alpha-beta win test for the attacker within a node budget: 1 a win, 0 no win, or
    // PROBE_RAN_OUT.
    int probe(const Position& pos, uint64_t budget) {
        probes++;
        bool attackerToMove = pos.sideToMove == attacker;
        int value = attackerToMove ? solver.searchWithin(pos, 0, 1, budget) : solver.searchWithin(pos, -1, 0, budget);
        if (value == Solver::UNKNOWN) {
            if (solver.pastDeadline()) aborted = true;
            return PROBE_RAN_OUT;
        }
        return (attackerToMove ? value >= 1 : value <= -1) ? 1 : 0;
    }

    int fullTest(const Position& pos) {
        int result = probe(pos, UINT64_MAX);
        return result == PROBE_RAN_OUT ? 0 : result;
    }

    // The moves of the side to move that no threat zone already refutes (empty: every move fails),
    // best first by the evaluation if `sorted`. `hadLegalMove` tells whether there was a legal move
    // at all; if not, the list holds the pass.
    void unrefutedMoves(const Position& pos, std::vector<Move>& out, bool& hadLegalMove, bool sorted) {
        int alpha = pos.sideToMove == attacker ? 0 : -1;  // the side to move's window is (0, 1) or (-1, 0)
        ThreatList threats;
        if (pos.piecesLeft[pos.sideToMove ^ 1]) threats.scan(pos, alpha, redBonusFor(pos.sideToMove));
        out.clear();
        hadLegalMove = false;
        std::vector<std::pair<double, Move>> scored;
        forEachLegalMove(pos, [&](Move m) {
            hadLegalMove = true;
            if (threats.size() && threats.boundFor(m.placement().cells) <= alpha) return;
            if (!sorted) { out.push_back(m); return; }
            Position child = pos;
            child.play(m);
            orderingNodes++;
            scored.push_back({child.isOver() ? -gameResult(child) * 1000.0 : -learnedLogit(child), m});
        });
        std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (auto& [score, m] : scored) out.push_back(m);
        if (!hadLegalMove) out.push_back(Move::pass());
    }

    // Reorders moves sorted by the evaluation so that every prefix is a sample spread over the
    // whole list: the k-th pick is the move at quantile vdc(k) + u (van der Corput sequence, with
    // the offset u drawn from the seed, so that runs are reproducible). Stratifying by the
    // evaluation lowers the variance of the estimates, since the hard replies are the ones the
    // evaluation rates best for the defender.
    static void spreadSampleOrder(std::vector<Move>& moves, uint64_t seed) {
        int n = (int)moves.size();
        std::vector<char> taken(n, 0);
        std::vector<Move> spread;
        double offset = (splitmix64(seed) >> 11) * (1.0 / 9007199254740992.0);
        for (int k = 0; k < n; k++) {
            double quantile = offset, weight = 0.5;
            for (int bits = k; bits; bits >>= 1, weight *= 0.5)
                if (bits & 1) quantile += weight;
            int i = std::min(n - 1, int((quantile - (int)quantile) * n));
            while (taken[i]) i = (i + 1) % n;
            taken[i] = 1;
            spread.push_back(moves[i]);
        }
        moves.swap(spread);
    }

    struct Candidate {
        Move move;
        std::vector<Move> replies;  // in sample order: the first `probed` ones have been probed
        int probed = 0, hard = 0;
        bool refuted = false;       // a sampled reply (or the game's end) is no attacker win
        double estimate() const { return refuted ? INFINITY : replies.size() * (hard + 0.5) / (probed + 1.0); }
    };

    // Attacker to move: 1 if some move wins, 0 if none does.
    int someMoveWins(const Position& pos) {
        if (piecesLeft(pos) < screenFromPieces || pos.isOver()) return fullTest(pos);
        std::vector<Move> moves;
        bool hadLegalMove;
        unrefutedMoves(pos, moves, hadLegalMove, true);
        if (!hadLegalMove) {
            Position child = pos;
            child.play(Move::pass());
            return everyReplyLoses(child);
        }

        // the candidates: the best moves by the evaluation, with their replies in sample order
        screens++;
        int candidateCount = std::min<int>(screenedMoves, (int)moves.size());
        std::vector<Candidate> candidates(candidateCount);
        std::vector<Candidate*> field;
        for (int i = 0; i < candidateCount; i++) {
            Candidate& c = candidates[i];
            c.move = moves[i];
            Position child = pos;
            child.play(c.move);
            if (child.isOver()) {
                if (fullTest(child)) return 1;
                c.refuted = true;
                continue;
            }
            bool defenderHadMove;
            unrefutedMoves(child, c.replies, defenderHadMove, true);
            if (defenderHadMove && c.replies.empty()) return 1;  // the threats refute every defender reply
            spreadSampleOrder(c.replies, child.hash[0] ^ sampleSeed);
            field.push_back(&c);
        }

        // the screening rounds
        uint64_t hardBudget = budgetFor(hardReplyNodes, piecesLeft(pos) - 2);
        auto byEstimate = [](const Candidate* a, const Candidate* b) { return a->estimate() < b->estimate(); };
        for (int round = 0; round < ROUNDS && !field.empty(); round++) {
            for (Candidate* c : field) {
                Position child = pos;
                child.play(c->move);
                for (int j = 0; j < SAMPLES_PER_ROUND[round] && c->probed < (int)c->replies.size() && !c->refuted; j++) {
                    Position reply = child;
                    reply.play(c->replies[c->probed++]);
                    int result = probe(reply, hardBudget);
                    if (aborted) return 0;
                    if (result == 0) c->refuted = true;
                    else if (result == PROBE_RAN_OUT) c->hard++;
                }
            }
            field.erase(std::remove_if(field.begin(), field.end(), [](Candidate* c) { return c->refuted; }), field.end());
            std::stable_sort(field.begin(), field.end(), byEstimate);
            if (round + 1 < ROUNDS) field.resize((field.size() + 1) / 2);
        }
        if (trace)
            for (auto& c : candidates)
                printf("  ply %d screen %-18s replies %3zu hard %2d/%2d%s\n", (int)pos.ply, moveToString(c.move).c_str(), c.replies.size(),
                       c.hard, c.probed, c.refuted ? " refuted" : "");

        // the proofs: the last round's survivors first, then the other screened moves by their
        // estimate, then the remaining moves by the evaluation
        std::vector<Candidate*> order(field);
        for (auto& c : candidates)
            if (!c.refuted && std::find(field.begin(), field.end(), &c) == field.end()) order.push_back(&c);
        std::stable_sort(order.begin() + field.size(), order.end(), byEstimate);
        for (Candidate* c : order) {
            Position child = pos;
            child.play(c->move);
            if (everyReplyLoses(child)) return 1;
            if (aborted) return 0;
        }
        for (int i = candidateCount; i < (int)moves.size(); i++) {
            Position child = pos;
            child.play(moves[i]);
            if (fullTest(child)) return 1;
            if (aborted) return 0;
        }
        return 0;
    }

    // Defender to move: 1 if every reply loses for it, 0 if one does not. Replies that are
    // expensive attacker nodes themselves (their probe runs out) are screened in turn.
    int everyReplyLoses(const Position& pos) {
        if (piecesLeft(pos) - 1 < screenFromPieces || pos.isOver()) return fullTest(pos);
        std::vector<Move> replies;
        bool hadLegalMove;
        unrefutedMoves(pos, replies, hadLegalMove, true);
        if (hadLegalMove && replies.empty()) return 1;
        uint64_t budget = budgetFor(2 * hardReplyNodes, piecesLeft(pos) - 1);
        for (Move m : replies) {
            Position child = pos;
            child.play(m);
            int result = probe(child, budget);
            if (result == PROBE_RAN_OUT && !aborted) result = someMoveWins(child);
            if (result <= 0) return 0;
        }
        return 1;
    }
};
