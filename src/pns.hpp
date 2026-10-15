// Win test that looks for cheap proofs first: does player A (the attacker) win?
//
// The alpha-beta win test commits to the first attacker move that works, although other
// winning moves often have much cheaper proofs: a move's proof costs the sum over the
// defender's replies, and the typical reply cost differs ~10x between moves in ways the eval
// does not see. At an expensive attacker node the best moves by the eval are screened instead:
// a sample of each move's replies (spread over the eval order) gets alpha-beta probes with a
// small node budget, and replies x fraction of the sample above the budget estimates the
// move's proof number (how many hard replies its proof has). The samples grow in rounds while
// the field of moves is halved (successive halving); a sampled reply that refutes a move drops
// it at once. The moves are then proved in order of the estimate.
// Everything is decided by exact alpha-beta win tests (Solver, one TT shared by all probes),
// so budgets and samples only change which proof is looked for first, never the answer.
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "solver.hpp"

struct PnSearch {
    Solver ab;
    int attacker = 0;
    bool aborted = false;
    uint64_t probes = 0, screens = 0, pnNodes = 0;
    int screenRem = envInt("PNS_SCREEN_REM", 10);    // screen attacker nodes with at least this many pieces left
    int candidates = envInt("PNS_CANDIDATES", 8);    // moves screened per node
    uint64_t hardBudget = envInt("PNS_HARD", 1000);  // a reply with 8 pieces left is hard above this
    uint64_t sampleSeed = envInt("PNS_SEED", 0);      // other samples (to measure their spread)
    static constexpr int ROUNDS = 3;
    static constexpr int SAMPLES[ROUNDS] = {8, 8, 16};  // replies probed per move and round
    bool trace = envInt("PNS_TRACE", 0) != 0;

    // The probes order moves without history scores and with killers behind the eval: what made
    // cutoffs in one subtree picks expensive (if winning) moves in the next, and the win test
    // needs about half the nodes without it (SOLVER_HISTORY / SOLVER_KILLERS_OVER_EVAL override).
    explicit PnSearch(int log2size = 22) : ab(log2size) {
        ab.killersOverEval = envInt("SOLVER_KILLERS_OVER_EVAL", 0) != 0;
    }
    void clear() { ab.clear(); }
    uint64_t nodes() const { return ab.nodes + pnNodes; }
    static int pieces(const Pos& s) { return pieceCount(s.avail[0]) + pieceCount(s.avail[1]); }
    // node budgets follow the typical cost of a node: ~10x per two more pieces left
    static uint64_t scaled(uint64_t b, int rem) {
        for (; rem < 8; rem += 2) b /= 10;
        for (; rem > 9; rem -= 2) b *= 10;
        return std::max<uint64_t>(b, 16);
    }

    // Alpha-beta win test for the attacker within a node budget: 1 win, 0 no win, -1 budget exhausted.
    int probe(const Pos& s, uint64_t budget) {
        probes++;
        ab.aborted = false;
        ab.nodeLimit = budget == UINT64_MAX ? UINT64_MAX : ab.nodes + budget;
        bool att = s.stm == attacker;
        int v = att ? ab.search(s, 0, 1) : ab.search(s, -1, 0);
        if (ab.aborted) {
            if (ab.useDeadline && std::chrono::steady_clock::now() > ab.deadline) aborted = true;
            return -1;
        }
        return (att ? v >= 1 : v <= -1) ? 1 : 0;
    }
    int full(const Pos& s) { int r = probe(s, UINT64_MAX); return r < 0 ? 0 : r; }

    // Moves of the side to move that a threat zone does not already refute (empty: every move
    // fails), best first by the eval if `sorted`; `any` tells whether it had a legal move at all
    // (if not, the list holds the pass).
    void moves(const Pos& s, std::vector<Move>& out, bool& any, bool sorted) {
        int alpha = s.stm == attacker ? 0 : -1;  // the side to move's window is (0, 1) or (-1, 0)
        int bonus = s.stm == 0 ? RED_BONUS : -RED_BONUS;
        Solver::Threat threats[Solver::MAX_THREATS];
        int nThreats = 0;
        if (s.avail[s.stm ^ 1]) Solver::scanThreats(s, alpha, bonus, threats, nThreats);
        out.clear();
        any = false;
        std::vector<std::pair<double, Move>> sc;
        s.forEachLegal([&](Move m) {
            any = true;
            if (nThreats && Solver::threatBound(m.pl().m, threats, nThreats) <= alpha) return;
            if (!sorted) { out.push_back(m); return; }
            Pos c = s; c.play(m);
            pnNodes++;
            sc.push_back({c.over() ? -Solver::terminalValue(c) * 1000.0 : -learnedLogit(c), m});
        });
        std::stable_sort(sc.begin(), sc.end(), [](auto& a, auto& b) { return a.first > b.first; });
        for (auto& [v, m] : sc) out.push_back(m);
        if (!any) out.push_back(Move{});
    }

    // Reorders eval-sorted moves so that every prefix is a sample spread over the whole range:
    // the k-th move is the one at quantile vdc(k) + u (van der Corput sequence, offset u drawn
    // from the seed, so runs are reproducible). Stratifying by the eval lowers the variance of the
    // estimates, since hard replies are the ones the eval rates best for the defender.
    static void sampleOrder(std::vector<Move>& mv, uint64_t seed) {
        int n = (int)mv.size();
        std::vector<char> taken(n, 0);
        std::vector<Move> out;
        double u = (splitmix(seed) >> 11) * (1.0 / 9007199254740992.0);
        for (int k = 0; k < n; k++) {
            double q = u, f = 0.5;
            for (int b = k; b; b >>= 1, f *= 0.5) if (b & 1) q += f;
            int i = std::min(n - 1, int((q - (int)q) * n));
            while (taken[i]) i = (i + 1) % n;
            taken[i] = 1;
            out.push_back(mv[i]);
        }
        mv.swap(out);
    }

    struct Candidate {
        Move m;
        std::vector<Move> replies;  // in sample order: the first `used` were probed
        int used = 0, hard = 0;
        bool refuted = false;       // a sampled reply (or the game end) is no attacker win
        double estimate() const { return refuted ? INFINITY : replies.size() * (hard + 0.5) / (used + 1.0); }
    };

    // Attacker to move: 1 if some move wins, 0 if none does.
    int solveOr(const Pos& s) {
        if (pieces(s) < screenRem || s.over()) return full(s);
        std::vector<Move> mv;
        bool any;
        moves(s, mv, any, true);
        if (!any) { Pos c = s; c.play(Move{}); return solveAnd(c); }
        screens++;
        int k = std::min<int>(candidates, (int)mv.size());
        std::vector<Candidate> cand(k);
        std::vector<Candidate*> field;
        for (int i = 0; i < k; i++) {
            Candidate& c = cand[i];
            c.m = mv[i];
            Pos p = s; p.play(c.m);
            if (p.over()) { if (full(p)) return 1; c.refuted = true; continue; }
            bool anyReply;
            moves(p, c.replies, anyReply, true);
            if (anyReply && c.replies.empty()) return 1;  // every defender reply loses
            sampleOrder(c.replies, p.h[0] ^ sampleSeed);
            field.push_back(&c);
        }
        uint64_t hb = scaled(hardBudget, pieces(s) - 2);
        auto byEstimate = [](const Candidate* a, const Candidate* b) { return a->estimate() < b->estimate(); };
        for (int r = 0; r < ROUNDS && !field.empty(); r++) {
            for (Candidate* c : field) {
                Pos p = s; p.play(c->m);
                for (int j = 0; j < SAMPLES[r] && c->used < (int)c->replies.size() && !c->refuted; j++) {
                    Pos q = p; q.play(c->replies[c->used++]);
                    int v = probe(q, hb);
                    if (aborted) return 0;
                    if (v == 0) c->refuted = true;
                    else if (v < 0) c->hard++;
                }
            }
            field.erase(std::remove_if(field.begin(), field.end(), [](Candidate* c) { return c->refuted; }), field.end());
            std::stable_sort(field.begin(), field.end(), byEstimate);
            if (r + 1 < ROUNDS) field.resize((field.size() + 1) / 2);
        }
        if (trace)
            for (auto& c : cand)
                printf("  ply %d screen %-18s replies %3zu hard %2d/%2d%s\n", (int)s.ply, moveStr(c.m).c_str(), c.replies.size(), c.hard, c.used,
                       c.refuted ? " refuted" : "");
        // the last round's survivors first, then the other screened moves, then the rest by the eval
        std::vector<Candidate*> order(field);
        for (auto& c : cand)
            if (!c.refuted && std::find(field.begin(), field.end(), &c) == field.end()) order.push_back(&c);
        std::stable_sort(order.begin() + field.size(), order.end(), byEstimate);
        for (Candidate* c : order) {
            Pos p = s; p.play(c->m);
            if (solveAnd(p)) return 1;
            if (aborted) return 0;
        }
        for (int i = k; i < (int)mv.size(); i++) {
            Pos p = s; p.play(mv[i]);
            if (full(p)) return 1;
            if (aborted) return 0;
        }
        return 0;
    }

    // Defender to move: 1 if every reply loses for it, 0 if one does not. Replies that are
    // expensive attacker nodes themselves (a probe runs out) are screened in turn.
    int solveAnd(const Pos& s) {
        if (pieces(s) - 1 < screenRem || s.over()) return full(s);
        std::vector<Move> rep;
        bool any;
        moves(s, rep, any, true);
        if (any && rep.empty()) return 1;
        uint64_t pb = scaled(2 * hardBudget, pieces(s) - 1);
        for (Move m : rep) {
            Pos p = s; p.play(m);
            int v = probe(p, pb);
            if (v < 0 && !aborted) v = solveOr(p);
            if (v <= 0) return 0;
        }
        return 1;
    }

    // Does player A win from s? 1 yes, 0 no, -9 out of time.
    int solveWin(const Pos& root, int A, double seconds = 0) {
        attacker = A;
        ab.nodes = 0;
        probes = screens = pnNodes = 0;
        aborted = false;
        ab.useDeadline = seconds > 0;
        if (ab.useDeadline) ab.deadline = std::chrono::steady_clock::now() + std::chrono::microseconds((long long)(seconds * 1e6));
        int r = root.stm == A ? solveOr(root) : solveAnd(root);
        return aborted ? -9 : r;
    }

    // Exact value for the side to move (-9 if out of time): does it win, and if not, does the
    // opponent? totalNodes counts both questions.
    uint64_t totalNodes = 0;
    int solveValue(const Pos& s, double seconds = 0) {
        auto t0 = std::chrono::steady_clock::now();
        int w = solveWin(s, s.stm, seconds);
        totalNodes = nodes();
        if (w != 0) return w;
        double left = seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (seconds > 0 && left <= 0) return -9;
        int l = solveWin(s, s.stm ^ 1, seconds > 0 ? left : 0);
        totalNodes += nodes();
        return l == 1 ? -1 : l == 0 ? 0 : -9;
    }
};
