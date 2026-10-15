// Exact solver: negamax alpha-beta over {-1, 0, +1} (loss/draw/win for the side
// to move), transposition table keyed by the symmetry-canonical hash, snake-length
// bounds for early cutoffs, threat zones, an exact closed form for the last pieces,
// TT-move + killer + static + learned-eval move ordering, enhanced transposition cutoffs
// (ETC). The TT is lockless and can be shared by threads.
// There is no history heuristic: the scores it accumulated over a long solve outweighed
// the static ordering (own snake first, blocks next) and cost about 30% more nodes.
#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include "core.hpp"
#include "eval.hpp"

// Lockless 2-way transposition table. Each slot stores key^data and data, so an entry
// torn by concurrent writes fails the key check instead of returning garbage.
// Slot 0 of a bucket keeps the entry with the most work behind it, slot 1 is always replaced.
struct LTT {
    struct Slot { std::atomic<uint64_t> k{0}, d{0}; };
    std::unique_ptr<Slot[]> s;
    size_t nb;
    uint64_t mask;
    explicit LTT(int log2buckets) : s(new Slot[size_t(2) << log2buckets]), nb(size_t(1) << log2buckets), mask(nb - 1) {}
    void clear() {
        for (size_t i = 0; i < 2 * nb; i++) { s[i].k.store(0, std::memory_order_relaxed); s[i].d.store(0, std::memory_order_relaxed); }
    }
    static uint64_t pack(int lo, int hi, int best, int work) {
        return uint64_t(lo + 2) | uint64_t(hi + 2) << 3 | uint64_t(best + 1) << 6 | uint64_t(work) << 22;
    }
    bool probe(uint64_t key, int& lo, int& hi, int& best) const {
        size_t b = (key & mask) * 2;
        for (int i = 0; i < 2; i++) {
            uint64_t k = s[b + i].k.load(std::memory_order_relaxed), d = s[b + i].d.load(std::memory_order_relaxed);
            if (d && (k ^ d) == key) {
                lo = int(d & 7) - 2; hi = int(d >> 3 & 7) - 2; best = int(d >> 6 & 0xFFFF) - 1;
                return true;
            }
        }
        return false;
    }
    void store(uint64_t key, int lo, int hi, int best, int work) {
        size_t b = (key & mask) * 2;
        uint64_t d = pack(lo, hi, best, work);
        uint64_t k0 = s[b].k.load(std::memory_order_relaxed), d0 = s[b].d.load(std::memory_order_relaxed);
        int i = ((k0 ^ d0) == key || work >= int(d0 >> 22 & 63)) ? 0 : 1;
        s[b + i].d.store(d, std::memory_order_relaxed);
        s[b + i].k.store(key ^ d, std::memory_order_relaxed);
    }
};

inline int envInt(const char* name, int def) { const char* v = getenv(name); return v ? atoi(v) : def; }
// SOLVER_RED_BONUS=b counts b extra cells for red at the end of the game (default 0 = the real rules).
// With it, "blue still wins" means blue wins by more than b: used to find the exact margin of a line.
inline const int RED_BONUS = envInt("SOLVER_RED_BONUS", 0);

enum { PART_TT, PART_LAST, PART_BOUNDS, PART_KILLER_THREATS, PART_MOVEGEN, PART_SCAN, PART_CHILD, NPART };
inline const char* PART_NAMES[NPART] = {"tt", "lastPieces", "bounds", "killerThreats", "movegen", "scan", "childMake"};

struct Solver {
    std::shared_ptr<LTT> tt;
    int16_t killer[32][2];
    uint64_t nodes = 0;
    std::atomic<bool>* stop = nullptr;
    std::chrono::steady_clock::time_point deadline;
    bool useDeadline = false;
    bool aborted = false;
    uint64_t nodeLimit = UINT64_MAX;
    // profiling counters per ply: visited, cut by TT, cut by bounds, expanded
    uint64_t pVisit[16] = {}, pTT[16] = {}, pBound[16] = {}, pExp[16] = {};
    uint64_t pPruned[16] = {}, pMoves[16] = {}, pLast[16] = {}, pScans[16] = {}, pScanHits[16] = {};  // ... / nodes solved by lastPieces  // moves skipped by threat zones / moves generated, per ply
    bool useThreats = envInt("SOLVER_THREATS", 1) != 0;
    bool useLastPieces = envInt("SOLVER_LAST", 1) != 0;
    bool threatScan = envInt("SOLVER_THREAT_SCAN", 1) != 0;
    int evalOrderRem = envInt("SOLVER_EVAL_REM", 7);  // order children by the learned evaluation when >= this many pieces are left
    // deeper down, eval-order only where the side to move does not look lost: the losing side
    // has to try every move anyway, so ordering its moves is wasted work
    int evalOrderRemWin = envInt("SOLVER_EVAL_WIN_REM", 7);
    double evalWinThreshold = envInt("SOLVER_EVAL_WIN_THR", -100) / 100.0;
    int etcRem = envInt("SOLVER_ETC_REM", 99);        // enhanced transposition cutoffs when >= this many pieces are left

    // log2size: log2 of the number of entries (16 bytes each)
    explicit Solver(int log2size = 22) : tt(std::make_shared<LTT>(log2size - 1)) { init(); }
    explicit Solver(std::shared_ptr<LTT> shared) : tt(std::move(shared)) { init(); }
    std::vector<Move> mvBuf[32];
    std::vector<int> scBuf[32];
    std::vector<uint64_t> sortBuf[32];
    static constexpr int FULL_SORT_AFTER = 3;
    void init() {
        for (int i = 0; i < 32; i++) { mvBuf[i].resize(3000); scBuf[i].resize(3000); sortBuf[i].resize(3000); }
        std::memset(killer, -1, sizeof killer);
    }
    void clear() {
        tt->clear();
        std::memset(killer, -1, sizeof killer);
    }

    static int terminalValue(const Pos& s) {
        int d = finalScore(s) + RED_BONUS;
        int v = d > 0 ? 1 : d < 0 ? -1 : 0;
        return s.stm == 0 ? v : -v;
    }

    // Threats. An opponent placement b is a threat when, if the opponent could play b right
    // now as a free move, the snake bounds would already decide the game. Any move of ours
    // that leaves b playable (does not cover b, and does not touch b when b starts a new
    // snake) lets the opponent answer with b, and the bounds only tighten as cells fill up
    // (our upper bound counts every cell we could still use, so it covers our move too).
    // Such moves are therefore worth at most the bound value, and only moves through the
    // zone of every threat strong enough for the current window need to be searched.
    struct Threat {
        BB zone;
        int value;  // upper bound on our value for every move that leaves the zone free
    };
    static constexpr int MAX_THREATS = 6, MAX_TRIED = 8, MAX_FOUND = 64;

    // Our value bound after the opponent plays b, or 2 if b is not a legal opponent placement.
    static int threatValue(const Pos& s, const Placement& b, int piece) {
        int me = s.stm, opp = me ^ 1;
        if (!(s.avail[opp] >> piece & 1) || !s.legalFor(opp, b)) return 2;
        Pos after = s;
        after.own[opp] |= b.m;
        int lbMe, ubMe;
        snakeBounds(after, me, lbMe, ubMe);
        ubMe += me == 0 ? RED_BONUS : -RED_BONUS;
        int lbOpp = lengthAfter(s, opp, b);
        return ubMe < lbOpp ? -1 : ubMe <= lbOpp ? 0 : 1;
    }
    static BB threatZone(const Pos& s, const Placement& b) {
        bool grows = b.nb & s.own[s.stm ^ 1];
        return grows ? b.m : b.m | b.nb;
    }
    // Adds the opponent placement gid as a threat if it is one (value <= 0).
    static void addThreat(const Pos& s, int gid, Threat* threats, int& n, int* tried, int& nTried) {
        if (gid < 0 || n >= MAX_THREATS || nTried >= MAX_TRIED) return;
        for (int i = 0; i < nTried; i++) if (tried[i] == gid) return;
        tried[nTried++] = gid;
        auto [p, idx] = T.gidInfo[gid];
        const Placement& b = T.PL[p][idx];
        int v = threatValue(s, b, p);
        if (v <= 0) threats[n++] = Threat{threatZone(s, b), v};
    }
    // Every opponent placement that is a threat for the current window, cheapest test first:
    // a placement only lowers our bound inside the clusters it covers, so every cluster whose
    // bound exceeds the opponent's length after the placement must be covered by it.
    // Keeps the smallest zones (a zone containing a kept one prunes nothing more).
    static void scanThreats(const Pos& s, int alpha, int bonus, Threat* threats, int& n) {
        Clusters cl;
        int lbMe, ubMe;
        snakeClusters(s, s.stm, lbMe, ubMe, cl);
        scanThreats(s, cl, alpha, bonus, threats, n);
    }
    // cl: our clusters (snakeClusters of the side to move)
    static void scanThreats(const Pos& s, const Clusters& cl, int alpha, int bonus, Threat* threats, int& n) {
        int me = s.stm, opp = me ^ 1;
        int lbMe = s.L[me];
        int strict = alpha < 0 ? 1 : 0;  // a loss needs our bound strictly below the opponent
        // a placement joins at most two snakes (it is one segment of a path), which caps what
        // the opponent can reach, and every cluster above that cap must be covered
        int top[2] = {0, 0};
        for (int j = 0; j < s.ncomp[opp]; j++) {
            int len = popc(s.comp[opp][j]);
            if (len > top[0]) { top[1] = top[0]; top[0] = len; }
            else if (len > top[1]) top[1] = len;
        }
        int maxNeed = std::max<int>(s.L[opp], 4 + top[0] + top[1]) - bonus - strict;
        if (lbMe > maxNeed) return;
        BB mustCover = ~(BB)0;
        for (int i = 0; i < cl.n; i++)
            if (cl.ub[i] > maxNeed) { mustCover = cl.cells[i] & ~s.own[me]; break; }
        Threat found[MAX_FOUND];
        int nf = 0;
        Pos asOpp = s;
        asOpp.stm = (uint8_t)opp;
        asOpp.forEachLegalTouching(mustCover, [&](Move b) {
            if (nf >= MAX_FOUND) return;
            const Placement& pl = b.pl();
            int lenOpp = lengthAfter(s, opp, pl);
            int need = lenOpp - bonus - strict;  // our raw upper bound must not exceed this
            if (lbMe > need) return;
            for (int i = 0; i < cl.n; i++)
                if (cl.ub[i] > need && !(cl.cells[i] & pl.m)) return;
            int ub = boundAfterBlock(s, me, cl, lbMe, pl.m) + bonus;
            int v = ub < lenOpp ? -1 : ub <= lenOpp ? 0 : 1;
            if (v <= alpha) found[nf++] = Threat{threatZone(s, pl), v};
        });
        std::sort(found, found + nf, [](const Threat& a, const Threat& b) { return popc(a.zone) < popc(b.zone); });
        for (int f = 0; f < nf && n < MAX_THREATS; f++) {
            bool redundant = false;
            for (int i = 0; i < n; i++)
                if ((threats[i].zone & ~found[f].zone) == 0 && threats[i].value <= found[f].value) redundant = true;
            if (!redundant) threats[n++] = found[f];
        }
    }
    // Upper bound on our value for a move covering cells m (2 = no bound from the threats).
    static int threatBound(BB m, const Threat* threats, int n) {
        int v = 2;
        for (int i = 0; i < n; i++)
            if (!(m & threats[i].zone)) v = std::min(v, threats[i].value);
        return v;
    }

    // Exact value when each player has at most one piece left (-2 otherwise), without a search:
    // the side to move places its piece, then the opponent makes its longest snake with the
    // placements that are still legal. A placement only ever removes opponent placements
    // (covered cells, or new snakes that would now touch it), never adds any.
    static int lastPieces(const Pos& s) {
        int me = s.stm, opp = me ^ 1;
        int nMe = pieceCount(s.avail[me]), nOpp = pieceCount(s.avail[opp]);
        if (nMe > 1 || nOpp > 1 || s.ply == 0) return -2;
        int bonus = me == 0 ? RED_BONUS : -RED_BONUS;
        auto result = [&](int lenMe, int lenOpp) { int d = lenMe + bonus - lenOpp; return d > 0 ? 1 : d < 0 ? -1 : 0; };
        // the opponent's placements that would lengthen its longest snake, longest first
        struct Reply { BB block; int len; };  // block: our cells that make the reply illegal
        Reply replies[512];
        int nr = 0;
        if (nOpp) {
            Pos asOpp = s;
            asOpp.stm = (uint8_t)opp;
            BB occ = s.occ();
            asOpp.forEachLegal([&](Move q) {
                const Placement& pl = q.pl();
                int len = lengthAfter(s, opp, pl);
                if (len > s.L[opp] && nr < 512) replies[nr++] = Reply{pl.nb & occ ? pl.m : pl.m | pl.nb, len};
            });
            std::sort(replies, replies + nr, [](const Reply& a, const Reply& b) { return a.len > b.len; });
        }
        auto oppBest = [&](BB ours) {
            for (int i = 0; i < nr; i++) if (!(replies[i].block & ours)) return replies[i].len;
            return (int)s.L[opp];
        };
        int best = -2;
        if (nMe) {
            s.forEachLegal([&](Move b) {
                if (best == 1) return;
                const Placement& pl = b.pl();
                best = std::max(best, result(lengthAfter(s, me, pl), oppBest(pl.m)));
            });
        }
        if (best == -2) best = result(s.L[me], oppBest(0));  // no piece or no legal placement: pass
        return best;
    }

    bool checkAbort() {
        if (aborted) return true;
        if ((nodes & 4095) == 0) {
            if (stop && stop->load(std::memory_order_relaxed)) aborted = true;
            else if (useDeadline && std::chrono::steady_clock::now() > deadline) aborted = true;
        }
        if (nodes > nodeLimit) aborted = true;
        return aborted;
    }

#ifdef SOLVER_TIMING
    // exclusive time per ply (build with -DSOLVER_TIMING): each node charges its own time
    // minus the time of its children; pPart splits it into the steps named in PART_NAMES
    uint64_t pTime[16] = {}, childTime = 0, pPart[16][NPART] = {};
    struct PlyTimer {
        Solver& S; int pp; uint64_t t0, savedChild;
        PlyTimer(Solver& s_, int p) : S(s_), pp(p), t0(__builtin_ia32_rdtsc()), savedChild(s_.childTime) { S.childTime = 0; }
        ~PlyTimer() {
            uint64_t incl = __builtin_ia32_rdtsc() - t0;
            S.pTime[pp] += incl - S.childTime;
            S.childTime = savedChild + incl;
        }
    };
#endif
#ifdef SOLVER_TIMING
#define TIMED(part, stmt) do { uint64_t t_ = __builtin_ia32_rdtsc(); stmt; pPart[pp][part] += __builtin_ia32_rdtsc() - t_; } while (0)
#else
#define TIMED(part, stmt) do { stmt; } while (0)
#endif
    int search(const Pos& s, int alpha, int beta) {
#ifdef SOLVER_TIMING
        PlyTimer timer(*this, std::min<int>(s.ply, 15));
#endif
        nodes++;
        if (checkAbort()) return 0;
        if (s.over()) return terminalValue(s);
        uint64_t n0 = nodes;
        int pp = std::min<int>(s.ply, 15);
        pVisit[pp]++;

        int lo = -1, hi = 1;
        int t;
        uint64_t k = s.key(t);
        int ttMove = -1, eLo, eHi, eBest = -1;
        bool hit;
        TIMED(PART_TT, hit = tt->probe(k, eLo, eHi, eBest));
        if (hit) {
            lo = eLo; hi = eHi;
            if (lo >= hi || lo >= beta) { pTT[pp]++; return lo; }
            if (hi <= alpha) { pTT[pp]++; return hi; }
            if (eBest >= 0) ttMove = T.gidSym[eBest][T.inv[t]];
        }
        if (useLastPieces) {
            int v;
            TIMED(PART_LAST, v = lastPieces(s));
            if (v != -2) {
                pLast[pp]++;
                tt->store(k, v, v, eBest, 0);
                return v;
            }
        }
        // snake-length bound cutoffs
        int lbMe, ubMe, lbOp, ubOp;
        Clusters myClusters;  // kept for the threat scan
        TIMED(PART_BOUNDS, if (useThreats) snakeClusters(s, s.stm, lbMe, ubMe, myClusters); else snakeBounds(s, s.stm, lbMe, ubMe);
                           snakeBounds(s, s.stm ^ 1, lbOp, ubOp));
        int bonus = s.stm == 0 ? RED_BONUS : -RED_BONUS;  // the side to move's share of SOLVER_RED_BONUS
        lbMe += bonus;
        ubMe += bonus;
        int blo = lbMe > ubOp ? 1 : lbMe >= ubOp ? 0 : -1;
        int bhi = ubMe < lbOp ? -1 : ubMe <= lbOp ? 0 : 1;
        lo = std::max(lo, blo);
        hi = std::min(hi, bhi);
        if (lo >= hi || lo >= beta) { pBound[pp]++; return lo; }
        if (hi <= alpha) { pBound[pp]++; return hi; }
        pExp[pp]++;
        alpha = std::max(alpha, lo);
        beta = std::min(beta, hi);
        int a0 = alpha, b0 = beta;

        // generate + order
        // per-depth move buffers (no per-node initialisation of 3000-element arrays)
        Move* moves = mvBuf[s.ply & 31].data();
        int* score = scBuf[s.ply & 31].data();
        int n = 0;
        BB me = s.own[s.stm];
        BB opp = s.own[s.stm ^ 1];
        BB oppFront = expand(shapeOf(opp).ends) & s.empty();
        int ply = std::min<int>(s.ply, 31);
        int k1 = killer[ply][0], k2 = killer[ply][1];
        // threats: the opponent's refutations one ply down are the natural candidates
        Threat threats[MAX_THREATS];
        int nThreats = 0, tried[MAX_TRIED], nTried = 0;
        int skipped = -2;  // best bound of the moves skipped because of threats
        bool threatsOn = useThreats && s.avail[s.stm ^ 1] && ply < 31;
        if (threatsOn) {
            TIMED(PART_KILLER_THREATS, addThreat(s, killer[ply + 1][0], threats, nThreats, tried, nTried);
                                       addThreat(s, killer[ply + 1][1], threats, nThreats, tried, nTried));
        }
#ifdef SOLVER_TIMING
        uint64_t genT0 = __builtin_ia32_rdtsc();
#endif
        bool scanned = !threatsOn || !threatScan;
        s.forEachLegal([&](Move m) {
            const Placement& pl = m.pl();
            if (nThreats) {
                int tb = threatBound(pl.m, threats, nThreats);
                if (tb <= alpha) { skipped = std::max(skipped, tb); pPruned[pp]++; return; }
            }
            int sc = PIECE_CELLS[m.piece()] * 64;
            if (pl.nb & me) sc += 256;
            if (pl.m & oppFront) sc += 128;
            else if (pl.nb & opp) sc += 32;
            if (m.gid == ttMove) sc = 1 << 30;
            else if (m.gid == k1) sc = 1 << 29;
            else if (m.gid == k2) sc = 1 << 28;
            moves[n] = m; score[n] = sc; n++;
        });
#ifdef SOLVER_TIMING
        pPart[pp][PART_MOVEGEN] += __builtin_ia32_rdtsc() - genT0;
#endif
        int rem = pieceCount(s.avail[0]) + pieceCount(s.avail[1]);
        bool doEval = n > 1 && (rem >= evalOrderRem || (rem >= evalOrderRemWin && learnedLogit(s) > evalWinThreshold));
        bool doEtc = n > 0 && rem >= etcRem;
        // the evaluation is expensive: when the table or a killer suggests a move, try it first and
        // evaluate the other moves only if it does not cut off
        bool evalLater = false;
        if (doEval && !doEtc)
            for (int i = 0; i < n && !evalLater; i++) evalLater = score[i] >= (1 << 28);
        if (evalLater) doEval = false;
        auto evaluateRest = [&](int from) {
            for (int i = from; i < n; i++)
                if (score[i] < (1 << 28)) {
                    Pos c = s; c.play(moves[i]);
                    score[i] = c.over() ? -terminalValue(c) * 100000 : -(int)(1000 * learnedLogit(c));
                }
        };
        if (doEval || doEtc) {
            for (int i = 0; i < n; i++) {
                Pos c = s; c.play(moves[i]);
                if (doEtc) {
                    // enhanced transposition cutoff: a child already known to be good enough
                    int cv = -9;
                    if (c.over()) cv = -terminalValue(c);
                    else { int ct, clo, chi, cb; if (tt->probe(c.key(ct), clo, chi, cb)) cv = -chi; }  // our value >= -chi
                    if (cv >= beta) {
                        tt->store(k, std::max(lo, cv), hi, T.gidSym[moves[i].gid][t], 0);
                        return cv;
                    }
                }
            }
            if (doEval) evaluateRest(0);
        }
        pMoves[pp] += n;
        int best = -2, bestGid = -1;
        if (n == 0 && skipped == -2) {
            Pos c = s;
            c.play(Move{});
            best = -search(c, -beta, -alpha);
        } else {
            for (int i = 0; i < n; i++) {
                if (evalLater && i == 1) { evaluateRest(1); evalLater = false; }
                if (i < FULL_SORT_AFTER) {  // pick the best remaining move: most nodes cut on the first one
                    int bi = i;
                    for (int j = i + 1; j < n; j++) if (score[j] > score[bi]) bi = j;
                    std::swap(moves[i], moves[bi]); std::swap(score[i], score[bi]);
                } else if (i == FULL_SORT_AFTER) {  // every move may be needed: sort the rest once
                    uint64_t* keys = sortBuf[s.ply & 31].data();
                    for (int j = i; j < n; j++) keys[j] = uint64_t(uint32_t(score[j]) ^ 0x80000000u) << 16 | uint16_t(moves[j].gid);
                    std::sort(keys + i, keys + n, std::greater<uint64_t>());
                    for (int j = i; j < n; j++) { moves[j].gid = int16_t(keys[j] & 0xFFFF); score[j] = int(uint32_t(keys[j] >> 16) ^ 0x80000000u); }
                }
                if (nThreats) {
                    int tb = threatBound(moves[i].pl().m, threats, nThreats);
                    if (tb <= alpha) { skipped = std::max(skipped, tb); pPruned[pp]++; continue; }
                }
                Pos c;
                TIMED(PART_CHILD, c = s; c.play(moves[i]));
                int v = -search(c, -beta, -alpha);
                if (aborted) return 0;
                if (v > best) { best = v; bestGid = moves[i].gid; }
                if (v > alpha) alpha = v;
                if (alpha >= beta) {
                    if (killer[ply][0] != moves[i].gid) { killer[ply][1] = killer[ply][0]; killer[ply][0] = moves[i].gid; }
                    break;
                }
                // the refutation just found may be a threat against the remaining moves; once the
                // first move failed, this is probably a node where every move must be refuted, so
                // look for all the opponent's threats
                if (threatsOn && i + 2 < n) {
                    addThreat(s, killer[ply + 1][0], threats, nThreats, tried, nTried);
                    if (!scanned) {
                        scanned = true;
                        int before = nThreats;
                        TIMED(PART_SCAN, scanThreats(s, myClusters, alpha, bonus, threats, nThreats));
                        pScans[pp]++;
                        if (nThreats > before) pScanHits[pp]++;
                    }
                }
            }
            best = std::max(best, skipped);
        }
        if (aborted) return 0;
        // store
        int nlo = lo, nhi = hi;
        if (best <= a0) nhi = std::min(nhi, best);
        else if (best >= b0) nlo = std::max(nlo, best);
        else nlo = nhi = best;
        uint64_t work = nodes - n0 + 1;
        int w = 63 - __builtin_clzll(work);
        tt->store(k, nlo, nhi, bestGid >= 0 ? T.gidSym[bestGid][t] : eBest, w);
        return best;
    }

    // Returns value for the side to move, or -9 if aborted. A narrower window answers a
    // yes/no question more cheaply: (0, 1) asks "does the side to move win?" (result 1 or
    // <= 0), (-1, 0) asks "does it avoid losing?" (result >= 0 or -1).
    // (parallel version: see parallelSolve below)
    int solve(const Pos& s, double seconds = 0, uint64_t maxNodes = UINT64_MAX, int alpha = -1, int beta = 1) {
        aborted = false;
        nodes = 0;
        nodeLimit = maxNodes;
        useDeadline = seconds > 0;
        if (useDeadline)
            deadline = std::chrono::steady_clock::now() +
                       std::chrono::microseconds((long long)(seconds * 1e6));
        int v = search(s, alpha, beta);
        return aborted ? -9 : v;
    }
};

enum SolveQuestion { EXACT, LOSS_TEST, WIN_TEST };
// Parallel exact solve: root moves (symmetry-reduced, ordered by the learned
// evaluation) are distributed over threads, each with its own solver/TT. Stops
// as soon as a winning move is found. perMove receives the value of every root
// move for the side to move (-9 = unknown).
// question: EXACT values, or only whether the side to move loses (LOSS_TEST: every child is
// a win test for the opponent; a root move is -1, or 0 for "does not lose"; stops at the first
// move that does not lose) or wins (WIN_TEST: a root move is 1, or 0 for "does not win"; stops
// at the first winning move). The yes/no questions are much cheaper than exact values.
#include <functional>
#include <mutex>
#include <thread>
struct ParallelSolveResult {
    int value = -9;  // for the side to move, -9 unknown
    uint64_t nodes = 0;
    double seconds = 0;
    std::vector<std::pair<Move, int>> perMove;
};
// known: root moves whose value is already known (e.g. from an interrupted run) - skipped.
// onResult: called (under a lock) for every newly solved root move.
inline ParallelSolveResult parallelSolve(const Pos& root, double seconds, int threads, int ttLog2 = 22,
                                         const std::vector<std::pair<Move, int>>& known = {},
                                         std::function<void(Move, int, uint64_t, double)> onResult = nullptr,
                                         SolveQuestion question = EXACT) {
    auto t0 = std::chrono::steady_clock::now();
    ParallelSolveResult res;
    std::vector<Move> all, mv;
    root.genMoves(all);
    if (all.empty()) all.push_back(Move{});
    std::vector<uint64_t> seen;
    for (Move m : all) {
        Pos c = root; c.play(m);
        int t; uint64_t k = c.key(t);
        if (std::find(seen.begin(), seen.end(), k) == seen.end()) { seen.push_back(k); mv.push_back(m); }
    }
    std::vector<double> ord(mv.size());
    for (size_t i = 0; i < mv.size(); i++) {
        Pos c = root; c.play(mv[i]);
        ord[i] = c.over() ? -Solver::terminalValue(c) * 1000.0 : -learnedLogit(c);
    }
    std::vector<size_t> idx(mv.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return ord[a] > ord[b]; });
    std::vector<int> val(mv.size(), -9);
    std::atomic<bool> stop{false};
    int enough = question == LOSS_TEST ? 0 : 1;  // a root move this good settles the root
    for (auto& [m, v] : known)
        for (size_t i = 0; i < mv.size(); i++)
            if (mv[i] == m) { val[i] = v; if (v >= enough) stop = true; }
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> nodes{0};
    std::mutex mu;
    threads = std::max(1, std::min<int>(threads, (int)mv.size()));
    std::vector<std::thread> th;
    // SOLVER_SHARED=<log2 entries>: one transposition table shared by all threads
    int sharedLog2 = envInt("SOLVER_SHARED", 0);
    std::shared_ptr<LTT> shared = sharedLog2 ? std::make_shared<LTT>(sharedLog2 - 1) : nullptr;
    auto work = [&] {
        std::unique_ptr<Solver> SP(shared ? new Solver(shared) : new Solver(ttLog2));
        Solver& S = *SP;
        S.stop = &stop;
        for (;;) {
            size_t k = next++;
            if (k >= idx.size() || stop) break;
            size_t i = idx[k];
            if (val[i] != -9) continue;  // known from a previous run
            Pos c = root; c.play(mv[i]);
            auto ts = std::chrono::steady_clock::now();
            double left = seconds - std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (left <= 0) break;
            // child windows: LOSS_TEST asks whether the opponent wins, WIN_TEST whether it avoids losing
            int v = question == LOSS_TEST ? S.solve(c, left, UINT64_MAX, 0, 1)
                    : question == WIN_TEST ? S.solve(c, left, UINT64_MAX, -1, 0)
                                           : S.solve(c, left);
            nodes += S.nodes;
            if (v == -9) continue;
            if (question == LOSS_TEST) v = v >= 1 ? 1 : 0;
            if (question == WIN_TEST) v = v <= -1 ? -1 : 0;
            std::lock_guard<std::mutex> lk(mu);
            val[i] = -v;
            if (-v >= enough) stop = true;
            if (onResult) onResult(mv[i], -v, S.nodes, std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());
        }
    };
    if (threads == 1) work();  // inline, so it also runs where there are no threads (WebAssembly)
    else {
        for (int w = 0; w < threads; w++) th.emplace_back(work);
        for (auto& t : th) t.join();
    }
    int best = -2;
    bool allKnown = true;
    for (size_t i = 0; i < mv.size(); i++) {
        res.perMove.push_back({mv[i], val[i]});
        if (val[i] == -9) allKnown = false; else best = std::max(best, val[i]);
    }
    if (best >= enough || (allKnown && best > -2)) res.value = best;
    res.nodes = nodes;
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}
