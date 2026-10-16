// Self-play training data for the network evaluation (`snek datagen`, see README).
// Every thread plays its own games with its own engine instance (the abEngine() singletons
// cannot play parallel games), from randomised openings, and labels the positions:
//   - the engine's own root verdict where the search proved the position (most of ply >= 6),
//   - otherwise an exact solve under a time limit for ply >= 6 and a sample of ply 5.
// Siblings (other children of the game positions) are the ranking targets: the search's best
// alternatives and random moves at plies 2-5, scored by a short search of their own; random moves of
// the ply-6 position, solved exactly; the root moves the search proved at plies 6-8.
// A solve that runs out of time is written as censored (exact = -8), never dropped: the
// positions the side to move loses take longest to prove, so dropping them biases the labels.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "core.hpp"
#include "mcts.hpp"
#include "search.hpp"
#include "solver.hpp"

// own0_own1_avail0_avail1_stm_ply_passes (the format of the label files in data/)
inline std::string hex128(BB b) {
    char buf[40];
    snprintf(buf, sizeof buf, "%016llx%016llx", (unsigned long long)(uint64_t)(b >> 64), (unsigned long long)(uint64_t)b);
    return buf;
}
inline BB parseHex128(const std::string& h) {
    BB r = 0;
    for (char c : h) r = r * 16 + (BB)(c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10);
    return r;
}
inline std::string posStr(const Pos& s) {
    return hex128(s.own[0]) + "_" + hex128(s.own[1]) + "_" + std::to_string(s.avail[0]) + "_" + std::to_string(s.avail[1]) +
           "_" + std::to_string(s.stm) + "_" + std::to_string(s.ply) + "_" + std::to_string(s.passes);
}
inline bool parsePosStr(const std::string& str, Pos& s) {
    std::vector<std::string> f;
    size_t i = 0;
    while (i <= str.size()) {
        size_t j = str.find('_', i);
        if (j == std::string::npos) j = str.size();
        f.push_back(str.substr(i, j - i));
        i = j + 1;
    }
    if (f.size() != 7) return false;
    s = Pos{};
    s.own[0] = parseHex128(f[0]); s.own[1] = parseHex128(f[1]);
    s.avail[0] = (uint8_t)std::stoi(f[2]); s.avail[1] = (uint8_t)std::stoi(f[3]);
    s.stm = (uint8_t)std::stoi(f[4]); s.ply = (uint8_t)std::stoi(f[5]); s.passes = (uint8_t)std::stoi(f[6]);
    s.initHash();
    return true;
}

// Exact labels in the data files, from the side to move's point of view.
enum ExactLabel { LABEL_LOSS = -1, LABEL_DRAW = 0, LABEL_WIN = 1, LABEL_CENSORED = -8, LABEL_UNKNOWN = -9 };

// The value of the root for the side to move when the analysis proved it, else LABEL_UNKNOWN.
inline int provenRootValue(const AbAnalysis& r) {
    bool allLost = !r.moves.empty();
    for (auto& m : r.moves) {
        if (m.verdict == Verdict::Win || (m.exact && m.score >= WIN / 2)) return LABEL_WIN;
        if (!(m.verdict == Verdict::Loss || m.score <= -WIN / 2)) allLost = false;
    }
    if (allLost) return LABEL_LOSS;
    if (verdictsSettle(r.moves)) return LABEL_DRAW;  // settled without a win or an all-loss: a proven draw
    return LABEL_UNKNOWN;
}

// Exact value with a time limit; LABEL_CENSORED when it ran out.
// The solver's table is kept between calls: its entries are exact facts about positions.
inline int exactLabel(Solver& S, const Pos& s, double seconds) {
    if (s.over()) return Solver::terminalValue(s);
    int v = S.solve(s, seconds);
    return v == -9 ? LABEL_CENSORED : v;
}

// Openings: one move list per line, anything after a # is a comment (blank lines are skipped).
inline std::vector<std::vector<Move>> readBook(const std::string& path) {
    std::vector<std::vector<Move>> book;
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { fprintf(stderr, "cannot open book %s\n", path.c_str()); exit(1); }
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        if (char* hash = strchr(line, '#')) *hash = 0;
        std::vector<Move> mv;
        Pos s = Pos::start();
        const char* sep = " \t\r\n";
        char* tok = strtok(line, sep);
        bool ok = true;
        for (; tok; tok = strtok(nullptr, sep)) {
            Move m;
            if (!parseMove(tok, m) || !s.legalMove(m)) { ok = false; break; }
            s.play(m);
            mv.push_back(m);
        }
        if (ok && !mv.empty()) book.push_back(mv);
    }
    fclose(f);
    return book;
}

// A fresh engine instance by name (one per thread for parallel games).
inline std::unique_ptr<AbEngine> makeEngine(const std::string& name) {
    if (name == "ab2") return std::make_unique<AbEngine>(abParamsV2());
    if (name == "ab") return std::make_unique<AbEngine>(AbParams{});
    if (isNnEngine(name)) {
        auto e = std::make_unique<AbEngine>(abParamsV2());
        e->net = nn::loadNet(nnPath(name));
        e->leaves = nnLeaves(NN_DEFAULT_LEAVES);
        return e;
    }
    fprintf(stderr, "unknown engine %s\n", name.c_str());
    exit(1);
}

struct DatagenConfig {
    int games = 100;
    int threads = 1;
    double seconds = 0.1;        // per move
    uint64_t seed = 1;
    double randomReplyShare = 0.5;  // random openings: share of games with a random reply to the 1-hex
    double randomShare = 0.12;   // plies 1-5: a uniformly random legal move
    double sampleShare = 0.3;    // plies 1-5: a move sampled from the top 4 by score
    double sampleTemp = 100;     // softmax temperature in score units (centi-logits)
    double ply5Share = 0.1;      // share of games whose ply-5 position gets an exact label
    double labelSeconds = 5;     // time limit of the exact labels at ply >= 6
    double ply5Seconds = 20;     // ... and at ply 5
    int siblings = 16;           // random children of the ply-6 position labelled exactly
    int provenKids = 16;         // children per position taken from the search's proven root moves
    int searchKids = 8;          // plies 2-5: children scored by a search (half the best alternatives, half random)
    double kidSeconds = 0.025;   // ... of this many seconds
    int leafSamples = 4;         // plies 2-5: depth-3 search leaves from the position, scored the same way
    std::vector<std::vector<Move>> book;  // openings (move lists); empty: random 1-hex + reply
};

// A move in a CSV field: its cells separated by '.' instead of ','.
inline std::string csvMove(Move m) {
    std::string s = moveStr(m);
    std::replace(s.begin(), s.end(), ',', '.');
    return s;
}

// One CSV row per position. kind g = game position (move = the move played); kind c = a sibling:
// the position after `move` from the game position at ply-1 (score and exact value as above);
// kind l = a search leaf three plies below a game position (score from a short search).
inline void printDatagenHeader(FILE* f) { fprintf(f, "kind,game,ply,stm,score,exact,result,margin,flag,move,pos\n"); }

template <class MakeEngine>
inline void runDatagen(const DatagenConfig& C, FILE* out, MakeEngine makeEngine) {
    std::mutex mu;
    std::atomic<int> nextGame{0};
    auto worker = [&](int w) {
        std::unique_ptr<AbEngine> E = makeEngine();
        Solver label(21);
        std::vector<Move> buf;
        for (;;) {
            int g = nextGame++;
            if (g >= C.games) break;
            Rng rng(C.seed * 1000003 + g);
            label.clear();
            Pos s = Pos::start();
            // kids: children of the position, with a search score and the exact value where known
            // (both for the child's side to move)
            struct Kid { Move mv; int score, exact; };
            struct Leaf { Pos s; int score, exact; };
            struct Rec { Pos s; int score, exact; char flag; Move mv; std::vector<Kid> kids; std::vector<Leaf> leaves; };
            std::vector<Rec> recs;
            auto playRec = [&](Move m, int score, int exact, char flag) {
                recs.push_back({s, score, exact, flag, m, {}, {}});
                s.play(m);
            };
            if (!C.book.empty()) {
                for (Move m : C.book[rng.below((int)C.book.size())]) playRec(m, 0, LABEL_UNKNOWN, 'o');
            } else {
                // a random 1-hex, then half the time a random reply (the engine's choice otherwise)
                s.genMoves(buf);
                playRec(buf[rng.below((int)buf.size())], 0, LABEL_UNKNOWN, 'o');
                if (rng.uni() < C.randomReplyShare) {
                    s.genMoves(buf);
                    playRec(buf[rng.below((int)buf.size())], 0, LABEL_UNKNOWN, 'o');
                }
            }
            while (!s.over()) {
                AbAnalysis r = abAnalyze(s, C.seconds, 1, 1, 64, *E);
                int exact = provenRootValue(r);
                int score = std::max(-WIN - 4000, std::min(WIN + 4000, r.moves[0].score));
                Move m = r.moves[0].mv;
                char flag = 'b';
                if (s.ply >= 1 && s.ply <= 5 && r.moves.size() > 1) {
                    double u = rng.uni();
                    if (u < C.randomShare) {
                        s.genMoves(buf);
                        m = buf[rng.below((int)buf.size())];
                        flag = 'r';
                    } else if (u < C.randomShare + C.sampleShare) {
                        int k = std::min<int>(4, (int)r.moves.size());
                        double wsum = 0, wt[4];
                        for (int i = 0; i < k; i++) {
                            double d = std::max(-3000, r.moves[i].score - r.moves[0].score);
                            wt[i] = std::exp(d / C.sampleTemp);
                            wsum += wt[i];
                        }
                        double x = rng.uni() * wsum;
                        int pick = 0;
                        while (pick < k - 1 && x > wt[pick]) { x -= wt[pick]; pick++; }
                        m = r.moves[pick].mv;
                        flag = pick ? 's' : 'b';
                    }
                }
                playRec(m, score, exact, flag);
                // the root moves the search proved at plies 5-7: exact children for free (s is the child now)
                for (auto& rm : r.moves) {
                    if (s.ply < 6 || s.ply > 8 || recs.back().kids.size() >= (size_t)C.provenKids || rm.mv == m || rm.mv.isPass()) continue;
                    int v = rm.verdict == Verdict::Win || (rm.exact && rm.score >= WIN / 2) ? LABEL_LOSS
                          : rm.verdict == Verdict::Loss || rm.score <= -WIN / 2 ? LABEL_WIN
                          : rm.verdict == Verdict::Draw ? LABEL_DRAW : LABEL_UNKNOWN;
                    if (v != LABEL_UNKNOWN) recs.back().kids.push_back({rm.mv, 0, v});
                }
                // plies 2-5: the best alternatives and random moves, each scored by a short search
                Pos parent = recs.back().s;
                if (parent.ply >= 2 && parent.ply <= 5 && C.searchKids > 0) {
                    std::vector<Move> pick;
                    for (auto& rm : r.moves)
                        if ((int)pick.size() < C.searchKids / 2 && !(rm.mv == m) && !rm.mv.isPass()) pick.push_back(rm.mv);
                    parent.genMoves(buf);
                    for (int k = 0; k < 4 * C.searchKids && (int)pick.size() < C.searchKids && !buf.empty(); k++) {
                        Move x = buf[rng.below((int)buf.size())];
                        if (!(x == m) && std::find(pick.begin(), pick.end(), x) == pick.end()) pick.push_back(x);
                    }
                    for (Move x : pick) {
                        Pos c = parent; c.play(x);
                        if (c.over()) { int v = Solver::terminalValue(c); recs.back().kids.push_back({x, terminalScore(c), v}); continue; }
                        AbAnalysis rc = abAnalyze(c, C.kidSeconds, 1, 1, 64, *E);
                        int sc = std::max(-WIN - 4000, std::min(WIN + 4000, rc.moves[0].score));
                        recs.back().kids.push_back({x, sc, provenRootValue(rc)});
                    }
                }
                // plies 2-5: leaves of a depth-3 search from the position, the kind of position the
                // evaluation scores during search: one of our moves (the search's best 8 or any), one of
                // the opponent's 24 best replies by the 1-ply evaluation (those the search keeps), any move
                if (parent.ply >= 2 && parent.ply <= 5)
                    for (int k = 0; k < C.leafSamples; k++) {
                        Pos x = parent;
                        bool ok = true;
                        for (int step = 0; step < 3 && ok; step++) {
                            x.genMoves(buf);
                            if (buf.empty()) { x.play(Move{}); ok = !x.over(); continue; }
                            Move y;
                            if (step == 0 && rng.uni() < 0.5) y = r.moves[rng.below(std::min<int>(8, (int)r.moves.size()))].mv;
                            else if (step == 1) {
                                std::vector<std::pair<int, Move>> sc;
                                for (Move z : buf) { Pos c = x; c.play(z); sc.push_back({c.over() ? -terminalScore(c) : -evaluate(c), z}); }
                                int keep = std::min<int>(24, (int)sc.size());
                                std::partial_sort(sc.begin(), sc.begin() + keep, sc.end(), [](auto& a, auto& b) { return a.first > b.first; });
                                y = sc[rng.below(keep)].second;
                            } else y = buf[rng.below((int)buf.size())];
                            if (y.isPass()) { ok = false; break; }
                            x.play(y);
                            ok = !x.over();
                        }
                        if (!ok) continue;
                        AbAnalysis rl = abAnalyze(x, C.kidSeconds, 1, 1, 64, *E);
                        recs.back().leaves.push_back({x, std::max(-WIN - 4000, std::min(WIN + 4000, rl.moves[0].score)), provenRootValue(rl)});
                    }
            }
            int finalL[2] = {s.L[0], s.L[1]};
            // exact labels where the search did not settle the position
            bool label5 = rng.uni() < C.ply5Share;
            for (auto& rc : recs) {
                if (rc.exact != LABEL_UNKNOWN || rc.s.over()) continue;
                if (rc.s.ply >= 6) rc.exact = exactLabel(label, rc.s, C.labelSeconds);
                else if (rc.s.ply == 5 && label5) rc.exact = exactLabel(label, rc.s, C.ply5Seconds);
            }
            // siblings of the ply-6 position: random legal moves, solved exactly
            for (auto& rc : recs) {
                if (rc.s.ply != 6 || C.siblings <= 0) continue;
                rc.s.genMoves(buf);
                for (int k = 0; k < C.siblings && !buf.empty(); k++) {
                    int i = rng.below((int)buf.size());
                    Move m = buf[i];
                    buf[i] = buf.back(); buf.pop_back();
                    Pos c = rc.s; c.play(m);
                    rc.kids.push_back({m, 0, exactLabel(label, c, C.labelSeconds)});
                }
            }
            std::string text;
            char line[512];
            long long gameId = (long long)C.seed * 1000000 + g;  // unique across runs with different seeds
            for (auto& rc : recs) {
                int me = rc.s.stm;
                int res = finalL[me] > finalL[me ^ 1] ? 1 : finalL[me] < finalL[me ^ 1] ? -1 : 0;
                snprintf(line, sizeof line, "g,%lld,%d,%d,%d,%d,%d,%d,%c,%s,%s\n", gameId, (int)rc.s.ply, me, rc.score, rc.exact, res,
                         finalL[me] - finalL[me ^ 1], rc.flag, csvMove(rc.mv).c_str(), posStr(rc.s).c_str());
                text += line;
                for (auto& lf : rc.leaves) {
                    snprintf(line, sizeof line, "l,%lld,%d,%d,%d,%d,-9,0,l,-,%s\n", gameId, (int)lf.s.ply, (int)lf.s.stm, lf.score, lf.exact,
                             posStr(lf.s).c_str());
                    text += line;
                }
                for (auto& k : rc.kids) {
                        Pos c = rc.s; c.play(k.mv);
                        snprintf(line, sizeof line, "c,%lld,%d,%d,%d,%d,-9,0,c,%s,%s\n", gameId, (int)c.ply, (int)c.stm, k.score, k.exact,
                                 csvMove(k.mv).c_str(), posStr(c).c_str());
                        text += line;
                    }
            }
            std::lock_guard<std::mutex> lk(mu);
            fputs(text.c_str(), out);
            fflush(out);
        }
    };
    std::vector<std::thread> th;
    for (int w = 0; w < C.threads; w++) th.emplace_back(worker, w);
    for (auto& t : th) t.join();
}
