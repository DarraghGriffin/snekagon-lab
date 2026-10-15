// The playing engine: iterative-deepening alpha-beta (principal variation search) on the
// learned evaluation. Unlike the exact solver its results are estimates, because it
//   - stops at a depth limit and uses the evaluation there,
//   - searches only the `width` most promising moves at nodes with depth >= 2 (forward pruning).
// Exact knowledge is used wherever it is cheap: snake-length bounds, and the exact solver once
// few pieces are left. The root moves are searched in parallel, with a multi-PV window so the
// UI gets real scores for the several best moves, not just the best one.
//
// There are two versions. "ab" is the first one. "ab2" adds exact verdicts on the root moves once
// at most 10 pieces are left (games between these engines are usually decided by plies 5-6, where
// the solver can tell winning moves apart within a second) and uses the whole move time for the
// search.
//
// Scores: centi-logits from the evaluation, or +-WIN (+ 100 per cell of margin) for proven results.
#pragma once
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include "solver.hpp"

constexpr int WIN = 100000;
constexpr int INF = 1000000;

struct EngineParams {
    int solvePiecesLeft = 6;       // try the exact solver when at most this many pieces are left (both players)
    uint64_t solveNodes = 20000;   // node budget for that attempt
    int width = 24;                // moves searched at nodes with depth >= 2 (best by 1-ply evaluation)
    double newIterationShare = 0.35;  // no iteration starts after this share of the search time
    bool partialIterations = false;   // an iteration cut off by the deadline still ranks the root moves it finished
    int exactVerdictsPiecesLeft = 0;  // exact root verdicts once at most this many pieces are left (0: never)
    int exactFirstPiecesLeft = 0;     // ... with a first exact phase in depth-1 order, before the heuristic search
    double exactFirstShare = 0.5;     // share of the move time for that first exact phase
    double heuristicShare = 0.35;     // share of the remaining time for the heuristic search; the exact phase gets the rest
};

// "ab2" ("ab" uses the defaults)
inline EngineParams engineParamsV2() {
    EngineParams p;
    p.newIterationShare = 0.6;
    p.partialIterations = true;
    p.exactVerdictsPiecesLeft = 10;
    p.exactFirstPiecesLeft = 9;
    return p;
}

// Score of a finished game for the side to move.
inline int finishedGameScore(const Position& pos) {
    int lead = pos.longestSnake[pos.sideToMove] - pos.longestSnake[pos.sideToMove ^ 1];
    return lead > 0 ? WIN + 100 * lead : lead < 0 ? -WIN + 100 * lead : 0;
}

inline bool isProvenScore(int score) { return std::abs(score) >= WIN / 2; }

// Logistic win probability of a score (proven results: 1 or 0).
inline double scoreToWinProb(int score) {
    if (score >= WIN / 2) return 1.0;
    if (score <= -WIN / 2) return 0.0;
    return 1.0 / (1.0 + std::exp(-score / 100.0));
}

class AlphaBetaSearcher {
  public:
    uint64_t nodes = 0;
    std::atomic<bool>* stopFlag = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool aborted = false;

    explicit AlphaBetaSearcher(const EngineParams& params, int log2Entries = 19) : params(params), solver(19) {
        table.assign(size_t(1) << log2Entries, Entry{0, 0, -1, 0, -1});
        mask = (uint64_t(1) << log2Entries) - 1;
        for (auto& h : history) h.assign(pieceTables.placements.size(), 0);
        std::memset(killers, -1, sizeof killers);
    }

    // Exact score when cheaply known (bounds decide the game, or the solver finishes within its
    // small budget).
    bool exactScore(const Position& pos, int& score) {
        SnakeBounds me = snakeBounds(pos, pos.sideToMove);
        SnakeBounds opp = snakeBounds(pos, pos.sideToMove ^ 1);
        if (me.lower > opp.upper) { score = WIN + 100 * (me.lower - opp.upper); return true; }
        if (me.upper < opp.lower) { score = -WIN - 100 * (opp.lower - me.upper); return true; }
        if (me.lower == me.upper && opp.lower == opp.upper && me.lower == opp.lower) { score = 0; return true; }
        int piecesLeft = pieceCount(pos.piecesLeft[RED]) + pieceCount(pos.piecesLeft[BLUE]);
        if (piecesLeft <= params.solvePiecesLeft) {
            int result = solver.solve(pos, 0, params.solveNodes);
            if (result != Solver::UNKNOWN) { score = result > 0 ? WIN : result < 0 ? -WIN : 0; return true; }
        }
        return false;
    }

    int search(const Position& pos, int depth, int alpha, int beta) {
        nodes++;
        if (shouldAbort()) return 0;
        if (pos.isOver()) return finishedGameScore(pos);
        if (depth <= 0) return evaluate(pos);

        int symmetry;
        uint64_t key = pos.canonicalKey(symmetry);
        Entry& entry = table[key & mask];
        int tableMove = -1;
        if (entry.key == key) {
            if (entry.depth >= depth) {
                if (entry.bound == EXACT) return entry.score;
                if (entry.bound == LOWER && entry.score >= beta) return entry.score;
                if (entry.bound == UPPER && entry.score <= alpha) return entry.score;
            }
            if (entry.bestMove >= 0) tableMove = pieceTables.symmetricPlacement[entry.bestMove][geometry.inverseSymmetry[symmetry]];
        }
        int exact;
        if (exactScore(pos, exact)) return exact;

        std::vector<Move> moves;
        generateMoves(pos, moves);
        if (moves.empty()) {
            Position child = pos;
            child.play(Move::pass());
            return -search(child, depth, -beta, -alpha);  // a pass does not use up depth
        }
        int count = (int)moves.size();
        std::vector<int> scores(count);
        if (depth >= 2) {
            if (!scoreByEvaluation(pos, moves, tableMove, scores)) return 0;  // aborted
        } else {
            scoreCheaply(pos, moves, tableMove, scores);
        }
        std::vector<int> order(count);
        for (int i = 0; i < count; i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return scores[a] > scores[b]; });

        // Forward pruning: at depth >= 2 only the best `width` moves - unless every move tried so
        // far is a proven loss (a pruned move might still save the game, and we must not report a
        // false forced win for the opponent).
        int searchLimit = depth >= 2 ? std::min(count, params.width) : count;
        int alphaAtStart = alpha, best = -INF, bestMove = -1;
        for (int k = 0; k < count; k++) {
            if (k >= searchLimit && best > -WIN / 2) break;
            Move m = moves[order[k]];
            Position child = pos;
            child.play(m);
            int value;
            if (k == 0) {
                value = -search(child, depth - 1, -beta, -alpha);
            } else {
                // principal variation search: prove with a null window that the move is not
                // better, re-search with the full window only if it is
                value = -search(child, depth - 1, -alpha - 1, -alpha);
                if (value > alpha && value < beta) value = -search(child, depth - 1, -beta, -alpha);
            }
            if (aborted) return 0;
            if (value > best) { best = value; bestMove = m.placementId; }
            if (value > alpha) alpha = value;
            if (alpha >= beta) {
                rememberCutoff(pos, m, depth);
                break;
            }
        }
        entry.key = key;
        entry.score = best;
        entry.depth = (int8_t)depth;
        entry.bound = best <= alphaAtStart ? UPPER : best >= beta ? LOWER : EXACT;
        entry.bestMove = bestMove >= 0 ? (int16_t)pieceTables.symmetricPlacement[bestMove][symmetry] : -1;
        return best;
    }

  private:
    enum Bound : uint8_t { LOWER = 1, UPPER = 2, EXACT = 3 };
    struct Entry {
        uint64_t key;
        int32_t score;
        int8_t depth;
        uint8_t bound;
        int16_t bestMove;  // canonical orientation
    };
    const EngineParams& params;
    std::vector<Entry> table;  // always-replace hash table
    uint64_t mask;
    Solver solver;
    std::vector<int32_t> history[2];
    int16_t killers[32][2];

    bool shouldAbort() {
        if (aborted) return true;
        // inner nodes are expensive (1-ply evaluation of all children), so check often
        if ((nodes & 63) == 0) {
            if ((stopFlag && stopFlag->load(std::memory_order_relaxed)) || std::chrono::steady_clock::now() > deadline) aborted = true;
        }
        return aborted;
    }

    // Depth >= 2: order by the evaluation after the move (table move first, killers boosted).
    // Returns false if the search was aborted meanwhile.
    bool scoreByEvaluation(const Position& pos, const std::vector<Move>& moves, int tableMove, std::vector<int>& scores) {
        int ply = std::min<int>(pos.ply, 31);
        for (size_t i = 0; i < moves.size(); i++) {
            if ((++nodes & 63) == 0 && shouldAbort()) return false;
            Position child = pos;
            child.play(moves[i]);
            scores[i] = child.isOver() ? -finishedGameScore(child) : -evaluate(child);
            if (moves[i].placementId == tableMove) scores[i] += 10 * WIN;
            else if (moves[i].placementId == killers[ply][0] || moves[i].placementId == killers[ply][1]) scores[i] += 200;
        }
        return true;
    }

    // Depth 1: cheap shape heuristics, like the exact solver.
    void scoreCheaply(const Position& pos, const std::vector<Move>& moves, int tableMove, std::vector<int>& scores) {
        int ply = std::min<int>(pos.ply, 31);
        Bitboard own = pos.ownCells();
        Bitboard opponentFront = neighboursOf(snakeShapeOf(pos.opponentCells()).ends) & pos.emptyCells();
        const std::vector<int32_t>& hist = history[pos.sideToMove];
        for (size_t i = 0; i < moves.size(); i++) {
            const Placement& p = moves[i].placement();
            int score = hist[moves[i].placementId] + PIECE_SIZE[p.piece] * 64;
            if (p.border & own) score += 512;
            if (p.cells & opponentFront) score += 256;
            if (moves[i].placementId == tableMove) score = 1 << 30;
            else if (moves[i].placementId == killers[ply][0]) score = 1 << 29;
            else if (moves[i].placementId == killers[ply][1]) score = 1 << 28;
            scores[i] = score;
        }
    }

    void rememberCutoff(const Position& pos, Move m, int depth) {
        history[pos.sideToMove][m.placementId] += depth * depth;
        int ply = std::min<int>(pos.ply, 31);
        if (killers[ply][0] != m.placementId) {
            killers[ply][1] = killers[ply][0];
            killers[ply][0] = m.placementId;
        }
    }
};

// ---- root moves ------------------------------------------------------------------------------

// Exact verdict on a root move for the side to move. NotWin: proven not to win (a draw or a loss).
enum class Verdict : int8_t { Unknown, Win, NotWin, Draw, Loss };

struct RootMoveResult {
    Move move;
    int score = 0;
    bool exact = false;  // the score is exact for this depth (not just an upper bound from the multi-PV window)
    int depth = 0;
    int display = 0;     // score averaged over the last two iterations (damps odd/even swings)
    Verdict verdict = Verdict::Unknown;  // from the exact phase (ab2)
    bool exactTried = false;             // the exact phase started on this move
};

struct EngineAnalysis {
    std::vector<RootMoveResult> moves;  // best first
    int depth = 0;
    uint64_t nodes = 0;
};

// One engine version with the state it keeps between moves: a searcher per thread (their tables
// stay warm) and the solvers of the exact root verdicts, which share one table (later positions of
// a game are subtrees of earlier ones, so proofs carry over).
struct EngineVersion {
    EngineParams params;
    std::vector<std::unique_ptr<AlphaBetaSearcher>> searchers;
    std::shared_ptr<TranspositionTable> verdictTable;
    std::vector<std::unique_ptr<Solver>> verdictSolvers;

    explicit EngineVersion(const EngineParams& params) : params(params) {}
};

// Version 1 is "ab", version 2 "ab2".
inline EngineVersion& engineVersion(int version) {
    static EngineVersion v1(EngineParams{}), v2(engineParamsV2());
    return version == 2 ? v2 : v1;
}

// Depth 1: every root move scored by exact knowledge where cheap, otherwise by the evaluation.
inline std::vector<RootMoveResult> scoreRootMovesAtDepth1(const Position& root, const std::vector<Move>& moves, AlphaBetaSearcher& searcher) {
    std::vector<RootMoveResult> results(moves.size());
    for (size_t i = 0; i < moves.size(); i++) {
        Position child = root;
        child.play(moves[i]);
        int score;
        if (child.isOver()) score = -finishedGameScore(child);
        else if (searcher.exactScore(child, score)) score = -score;
        else score = -evaluate(child);
        results[i].move = moves[i];
        results[i].score = score;
        results[i].exact = true;
        results[i].depth = 1;
        results[i].display = score;
    }
    std::stable_sort(results.begin(), results.end(), [](auto& a, auto& b) { return a.score > b.score; });
    return results;
}

// ---- the heuristic search of the root moves --------------------------------------------------

// One iteration at `depth` over all root moves, in parallel. The best `multipv` moves get exact
// scores; every other move is only shown to be worse than the multipv-th best so far. Moves proven
// lost keep their result. done[i] tells whether move i was finished before the deadline; returns
// false if not even the first (best so far) move was.
inline bool searchRootIteration(const Position& root, int depth, int threads, int multipv, EngineVersion& engine,
                                std::vector<RootMoveResult>& results, std::vector<char>& done) {
    auto& searchers = engine.searchers;
    int count = (int)results.size();
    for (auto& r : results)
        if (r.verdict != Verdict::Loss) {
            r.exact = false;
            r.depth = depth;
        }
    std::atomic<int> nextIndex{0};
    std::mutex lock;
    std::vector<int> exactScores;
    done.assign(count, 0);

    // the multipv-th best exact score so far: moves must beat it to need an exact score
    auto threshold = [&]() {
        std::lock_guard<std::mutex> guard(lock);
        if ((int)exactScores.size() < multipv) return -INF;
        std::vector<int> v = exactScores;
        std::nth_element(v.begin(), v.begin() + (multipv - 1), v.end(), std::greater<int>());
        return v[multipv - 1];
    };
    auto searchMove = [&](AlphaBetaSearcher& searcher, int i) {
        if (results[i].verdict == Verdict::Loss) { done[i] = 1; return; }  // proven: keeps its score
        Position child = root;
        child.play(results[i].move);
        int bar = threshold();
        int score;
        if (bar == -INF) {
            score = -searcher.search(child, depth - 1, -INF, INF);
        } else {
            score = -searcher.search(child, depth - 1, -bar - 1, -bar);
            if (!searcher.aborted && score > bar) score = -searcher.search(child, depth - 1, -INF, -bar);
        }
        if (searcher.aborted) return;
        std::lock_guard<std::mutex> guard(lock);
        results[i].score = score;
        results[i].exact = bar == -INF || score > bar;
        if (results[i].exact) exactScores.push_back(score);
        done[i] = 1;
    };
    auto worker = [&](int w) {
        AlphaBetaSearcher& searcher = *searchers[w];
        for (;;) {
            int i = nextIndex++;
            if (i >= count || searcher.aborted) break;
            searchMove(searcher, i);
        }
    };

    // the first (best so far) move alone sets the window, the rest in parallel
    searchMove(*searchers[0], nextIndex++);
    if (searchers[0]->aborted) return false;
    std::vector<std::thread> helpers;
    for (int w = 1; w < threads; w++) helpers.emplace_back(worker, w);
    worker(0);
    for (auto& t : helpers) t.join();
    return true;
}

// Odd and even depths are biased in opposite directions (the side that moved last looks better),
// so the displayed score is the average of the last two iterations where both are exact. Only
// the moves searched at `depth` get a new displayed score.
inline void smoothDisplayedScores(std::vector<RootMoveResult>& current, const std::vector<RootMoveResult>& previous, int depth) {
    for (auto& r : current) {
        if (r.depth != depth) continue;
        r.display = r.score;
        if (!r.exact || isProvenScore(r.score)) continue;
        for (auto& p : previous)
            if (p.move == r.move) {
                if (p.exact && !isProvenScore(p.score)) r.display = (r.score + p.score) / 2;
                break;
            }
    }
    // the value of the position (the best move) is smoothed even when the best move changed
    const RootMoveResult& previousBest = previous[0];
    RootMoveResult& best = current[0];
    if (best.exact && previousBest.exact && !isProvenScore(best.score) && !isProvenScore(previousBest.score))
        best.display = (best.score + previousBest.score) / 2;
}

// ---- exact root verdicts (ab2) ---------------------------------------------------------------

// Value of pos for the side to move from the exact solver with the window (alpha, beta), or
// Solver::UNKNOWN if the deadline or the stop flag came first.
inline int solveWithin(Solver& solver, const Position& pos, int alpha, int beta, std::chrono::steady_clock::time_point deadline,
                       std::atomic<bool>* stop) {
    solver.nodes = 0;
    if (pos.isOver()) return gameResult(pos);
    solver.stopFlag = stop;
    solver.setDeadline(deadline);
    return solver.searchWithin(pos, alpha, beta);
}

// The best root move is proven: a win, a draw once no move can win, or every move loses.
inline bool verdictsSettle(const std::vector<RootMoveResult>& moves) {
    bool allLost = !moves.empty(), open = false, draw = false;
    for (auto& r : moves) {
        if (r.verdict == Verdict::Win) return true;
        if (r.verdict != Verdict::Loss) allLost = false;
        if (r.verdict == Verdict::Unknown) open = true;
        if (r.verdict == Verdict::Draw) draw = true;
    }
    return allLost || (!open && draw);
}

// Exact verdicts on the root moves, in their current order (most promising first), until the
// deadline: first "does it win" for every move (a null window below a draw), then, once no move
// wins, "does it draw" for the others. Stops at the first win or draw: nothing better exists
// then. Moves are taken strictly in order: the winning move is usually among the first few, and
// node budgets per move (to skip expensive proofs) settled fewer positions in the same time.
// Returns the nodes searched.
inline uint64_t exactVerdicts(const Position& root, std::vector<RootMoveResult>& moves, std::chrono::steady_clock::time_point deadline,
                              int threads, EngineVersion& engine) {
    if (!engine.verdictTable) engine.verdictTable = std::make_shared<TranspositionTable>(22);  // 2^23 entries, 128 MB
    while ((int)engine.verdictSolvers.size() < threads) engine.verdictSolvers.emplace_back(new Solver(engine.verdictTable));
    std::atomic<uint64_t> nodes{0};
    for (int phase = 0; phase < 2; phase++) {
        if (verdictsSettle(moves)) break;
        bool drawPhase = phase == 1;
        Verdict todo = drawPhase ? Verdict::NotWin : Verdict::Unknown;
        // "does it draw" only once every move is known not to win
        if (drawPhase)
            for (auto& r : moves)
                if (r.verdict == Verdict::Unknown) return nodes;
        std::atomic<size_t> nextIndex{0};
        std::atomic<bool> stop{false};
        auto worker = [&](int w) {
            Solver& solver = *engine.verdictSolvers[w];
            for (;;) {
                size_t k = nextIndex++;
                if (k >= moves.size() || stop) break;
                RootMoveResult& r = moves[k];
                if (r.verdict != todo) continue;
                r.exactTried = true;
                Position child = root;
                child.play(r.move);
                // the opponent's value: below a draw (does our move win?), at most a draw (does it draw?)
                int value = drawPhase ? solveWithin(solver, child, 0, 1, deadline, &stop) : solveWithin(solver, child, -1, 0, deadline, &stop);
                nodes += solver.nodes;
                if (value == Solver::UNKNOWN) break;
                if (drawPhase) r.verdict = value <= 0 ? Verdict::Draw : Verdict::Loss;
                else r.verdict = value < 0 ? Verdict::Win : Verdict::NotWin;
                if (r.verdict == Verdict::Win || r.verdict == Verdict::Draw) stop = true;
            }
        };
        if (threads == 1) {
            worker(0);  // on this thread, so it also runs where there are no threads (the WebAssembly build of src/)
        } else {
            std::vector<std::thread> pool;
            for (int w = 0; w < threads; w++) pool.emplace_back(worker, w);
            for (auto& t : pool) t.join();
        }
    }
    return nodes;
}

// Reorders the moves as moves[0], the first of `other` not taken yet, moves[1], ... (each move once).
inline void interleave(std::vector<RootMoveResult>& moves, const std::vector<Move>& other) {
    std::vector<int> indexOf(pieceTables.placements.size() + 1, -1);  // placement id + 1 -> index in moves (a pass is -1)
    for (size_t i = 0; i < moves.size(); i++) indexOf[moves[i].move.placementId + 1] = (int)i;
    std::vector<char> taken(moves.size(), 0);
    std::vector<RootMoveResult> merged;
    merged.reserve(moves.size());
    size_t j = 0;
    for (size_t i = 0; i < moves.size(); i++) {
        if (!taken[i]) {
            taken[i] = 1;
            merged.push_back(moves[i]);
        }
        for (; j < other.size(); j++) {
            int k = indexOf[other[j].placementId + 1];
            if (k >= 0 && !taken[k]) {
                taken[k] = 1;
                merged.push_back(moves[k]);
                j++;
                break;
            }
        }
    }
    moves = merged;
}

// Moves proven lost go last (keeping their order), so the heuristic search never starts from one.
inline void lostMovesLast(std::vector<RootMoveResult>& moves) {
    std::stable_partition(moves.begin(), moves.end(), [](const RootMoveResult& r) { return r.verdict != Verdict::Loss; });
}

// The share of the opponent's replies to m that the verdict table already proves lost for the
// opponent: how far a running "does m win" proof got.
inline double proofProgress(const Position& root, Move m, const TranspositionTable& table) {
    Position child = root;
    child.play(m);
    int proven = 0, total = 0;
    forEachLegalMove(child, [&](Move reply) {
        Position after = child;
        after.play(reply);
        total++;
        int symmetry;
        TTEntry e;
        if (after.isOver() ? gameResult(after) > 0 : table.probe(after.canonicalKey(symmetry), e) && e.lower >= 1) proven++;
    });
    return total ? (double)proven / total : 0;
}

// The final ranking once verdicts are known: a proven win; the moves whose "does it win" proof was
// still running at the deadline, the furthest proof first (the exact phase refutes the candidates
// in order, so a winning move is usually one of them); a proven draw; the remaining moves (proven
// not to win, or never tried) in candidate order; proven losses. Scores are capped at what the
// verdict allows.
enum RankClass { PROVEN_WIN, PROOF_RUNNING, PROVEN_DRAW, NO_WIN_OR_UNTRIED, PROVEN_LOSS };
inline void rankByVerdict(const Position& root, std::vector<RootMoveResult>& moves, const EngineVersion& engine) {
    std::vector<double> progress(moves.size(), 0);
    for (size_t i = 0; i < moves.size(); i++) {
        RootMoveResult& r = moves[i];
        if (r.verdict == Verdict::Win) {
            r.score = r.display = WIN;
            r.exact = true;
        } else if (r.verdict == Verdict::Draw) {
            r.score = r.display = 0;
            r.exact = true;
        } else if (r.verdict == Verdict::NotWin) {
            r.score = std::min(r.score, 0);
            r.display = std::min(r.display, 0);
        } else if (r.verdict == Verdict::Unknown && r.exactTried && engine.verdictTable) {
            progress[i] = proofProgress(root, r.move, *engine.verdictTable);
        }
    }
    auto rankClass = [](const RootMoveResult& r) {
        switch (r.verdict) {
            case Verdict::Win: return PROVEN_WIN;
            case Verdict::Unknown: return r.exactTried ? PROOF_RUNNING : NO_WIN_OR_UNTRIED;
            case Verdict::Draw: return PROVEN_DRAW;
            case Verdict::NotWin: return NO_WIN_OR_UNTRIED;
            default: return PROVEN_LOSS;
        }
    };
    std::vector<size_t> order(moves.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        RankClass classA = rankClass(moves[a]), classB = rankClass(moves[b]);
        if (classA != classB) return classA < classB;
        return classA == PROOF_RUNNING && progress[a] > progress[b];
    });
    std::vector<RootMoveResult> ranked;
    for (size_t i : order) ranked.push_back(moves[i]);
    moves = ranked;
}

// ---- the analysis ----------------------------------------------------------------------------

inline EngineAnalysis analyzeWithEngine(const Position& root, double seconds, int threads, int multipv = 1, int maxDepth = 64,
                                        EngineVersion& engine = engineVersion(1)) {
    using Clock = std::chrono::steady_clock;
    const EngineParams& params = engine.params;
    auto startTime = Clock::now();
    auto deadline = startTime + std::chrono::microseconds((long long)(seconds * 1e6));
    std::atomic<bool> stop{false};
    auto& searchers = engine.searchers;
    while ((int)searchers.size() < threads) searchers.emplace_back(new AlphaBetaSearcher(params));

    std::vector<Move> moves = symmetryUniqueMoves(root);
    EngineAnalysis analysis;
    analysis.moves = scoreRootMovesAtDepth1(root, moves, *searchers[0]);
    analysis.depth = 1;
    if (moves.size() == 1) return analysis;
    std::vector<Move> evalOrder;  // the moves by their depth-1 score
    for (const RootMoveResult& r : analysis.moves) evalOrder.push_back(r.move);

    // Exact root verdicts after the heuristic search; with even fewer pieces left (the solver is
    // fast then) also a first exact phase in depth-1 order before it.
    int piecesLeft = pieceCount(root.piecesLeft[RED]) + pieceCount(root.piecesLeft[BLUE]);
    bool exactOn = params.exactVerdictsPiecesLeft && piecesLeft <= params.exactVerdictsPiecesLeft;
    bool exactFirst = exactOn && piecesLeft <= params.exactFirstPiecesLeft;
    if (exactFirst) {
        auto firstDeadline = startTime + std::chrono::microseconds((long long)(seconds * params.exactFirstShare * 1e6));
        analysis.nodes += exactVerdicts(root, analysis.moves, firstDeadline, threads, engine);
        lostMovesLast(analysis.moves);
    }

    // The heuristic search: all of the time, or its share of what is left when an exact phase follows.
    auto searchStart = exactOn ? Clock::now() : startTime;
    double timeLeft = seconds - std::chrono::duration<double>(searchStart - startTime).count();
    double searchSeconds = exactOn ? timeLeft * params.heuristicShare : seconds;
    auto searchDeadline = searchStart + std::chrono::microseconds((long long)(searchSeconds * 1e6));
    for (int i = 0; i < threads; i++) {
        searchers[i]->stopFlag = &stop;
        searchers[i]->deadline = searchDeadline;
        searchers[i]->aborted = false;
        searchers[i]->nodes = 0;
    }
    for (int depth = 2; depth <= maxDepth && !verdictsSettle(analysis.moves); depth++) {
        // an iteration takes a few times longer than the previous one: do not start one that
        // cannot finish
        double elapsed = std::chrono::duration<double>(Clock::now() - searchStart).count();
        if (elapsed > searchSeconds * params.newIterationShare) break;
        std::vector<RootMoveResult> next = analysis.moves;
        std::vector<char> done;
        if (!searchRootIteration(root, depth, threads, multipv, engine, next, done)) break;
        bool complete = std::find(done.begin(), done.end(), 0) == done.end();
        if (!complete && !params.partialIterations) break;
        // A cut-off iteration ranks the moves it finished (the previous best is always among them)
        // before the others, which keep the previous iteration's results.
        for (size_t i = 0; i < next.size(); i++)
            if (!done[i]) next[i] = analysis.moves[i];
        std::stable_sort(next.begin(), next.end(), [](auto& a, auto& b) {
            if (a.depth != b.depth) return a.depth > b.depth;
            if (a.exact != b.exact) return a.exact;
            return a.score > b.score;
        });
        smoothDisplayedScores(next, analysis.moves, depth);
        analysis.moves = next;
        analysis.depth = depth;
        if (!complete) break;
        if (next[0].exact && next[0].score >= WIN / 2) break;  // proven win found
        bool allLost = true;
        for (auto& r : next)
            if (r.score > -WIN / 2) allLost = false;
        if (allLost) break;
    }
    for (int i = 0; i < threads; i++) analysis.nodes += searchers[i]->nodes;

    if (exactOn) {
        // the candidates of the exact phase alternate between the search's and the depth-1
        // evaluation's favourites: each order often has the winning move near the top where the
        // other has not
        interleave(analysis.moves, evalOrder);
        lostMovesLast(analysis.moves);
        if (!verdictsSettle(analysis.moves) && Clock::now() < deadline)
            analysis.nodes += exactVerdicts(root, analysis.moves, deadline, threads, engine);
        rankByVerdict(root, analysis.moves, engine);
    }
    return analysis;
}
