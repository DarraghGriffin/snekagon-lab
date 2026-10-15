// snek refute <threads> <log> <move>...
// Shows that the side to move after the given moves (the attacker) cannot win - or, with env
// REFUTE_LOSS=1, that it loses - by refuting every attacker move: a defender reply after which
// the attacker does not win (with REFUTE_LOSS: loses). Finds an attacker move that survives instead
// if there is one.
//
//   pass 1 ("quick"): per attacker move, the defender replies that refuted other moves first, then
//            the defender's best replies by the evaluation; at most REFUTE_TRIES (12) win tests of
//            REFUTE_NODES million (60) nodes each. Attacker moves are spread over the threads.
//   pass 2 ("full"): per attacker move that pass 1 left open, attacker's best first, every defender
//            reply, most promising first, with a parallel yes/no solve - either a refutation or a
//            proof that the move survives, which settles the question.
// The passes alternate on chunks of REFUTE_CHUNK (64) attacker moves, so a strong move that
// survives is found without first refuting every weaker one; everything stops at the first move
// that survives.
//
// The log is appended to and read back on a restart (resume). Every refutation in it is one
// position anyone can solve again:
//   <move> refuted <reply> <nodes> <seconds>     (pass 2 logs no nodes and seconds)
//   <move> open                                  (pass 1 found no refutation)
//   <move> wins | holds                          (the move survives; holds: with REFUTE_LOSS)
#include <atomic>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include "commands.hpp"
#include "notation.hpp"
#include "parallel_solve.hpp"

int cmdRefute(const Args& args) {
    if (!args.has(4)) { printf("usage: snek refute <threads> <log> <moves...>\n"); return 1; }
    int threads = args.arg(2, 1);
    std::string logPath = args.arg(3, "");
    int maxTries = envInt("REFUTE_TRIES", 12);
    bool mustLose = envInt("REFUTE_LOSS", 0) != 0;
    uint64_t nodeBudget = (uint64_t)envInt("REFUTE_NODES", 60) * 1000000;
    Position pos = Position::initial();
    for (int i = 4; i < args.argc; i++)
        if (!playMoveText(pos, args.argv[i])) { printf("illegal move %s\n", args.argv[i]); return 1; }

    // what earlier runs found: the status of attacker moves, and how often each defender reply refuted one
    std::map<std::string, std::string> status;  // attacker move -> "refuted", "open", "wins" or "holds"
    std::map<int, int> refutations;             // defender reply (placement id) -> moves it refuted
    {
        std::ifstream in(logPath);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            std::string move, state, reply;
            if (!(fields >> move >> state)) continue;
            status[move] = state;
            Move replyMove;
            if (state == "refuted" && fields >> reply && parseMove(reply, replyMove)) refutations[replyMove.placementId]++;
        }
    }
    auto statusOf = [&](const std::string& move) {
        auto it = status.find(move);
        return it == status.end() ? std::string() : it->second;
    };
    FILE* log = fopen(logPath.c_str(), "a");

    // the attacker's moves, best first by the evaluation
    std::vector<Move> all = symmetryUniqueMoves(pos);
    std::vector<std::pair<double, Move>> ranked;
    for (Move m : all) {
        Position child = pos;
        child.play(m);
        ranked.push_back({child.isOver() ? -gameResult(child) * 1000.0 : -learnedLogit(child), m});  // the attacker's view
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<Move> todo;  // not settled by an earlier run
    for (auto& [score, m] : ranked)
        if (!status.count(moveToString(m))) todo.push_back(m);
    printf("%zu attacker moves, %zu settled earlier\n", all.size(), all.size() - todo.size());
    fflush(stdout);

    const char* survives = mustLose ? "holds" : "wins";
    std::string survivor;
    for (auto& [move, state] : status)
        if (state == survives) survivor = move;
    std::mutex lock;

    // Pass 1 on todo[from, to): a few node-limited win tests per move, moves spread over threads.
    auto quickPass = [&](size_t from, size_t to) {
        std::atomic<size_t> next{from};
        std::vector<std::thread> pool;
        for (int w = 0; w < threads; w++)
            pool.emplace_back([&] {
                Solver solver(22);
                for (;;) {
                    size_t k = next++;
                    if (k >= to) break;
                    Move m = todo[k];
                    Position afterMove = pos;
                    afterMove.play(m);
                    std::vector<Move> replies;
                    generateMoves(afterMove, replies);
                    // replies that refuted other moves first, then the ones that leave the attacker
                    // the lowest evaluation
                    std::vector<std::pair<double, Move>> order;
                    {
                        std::lock_guard<std::mutex> guard(lock);
                        for (Move reply : replies) {
                            auto known = refutations.find(reply.placementId);
                            Position child = afterMove;
                            child.play(reply);  // the attacker to move
                            double score = known != refutations.end() ? 1000.0 + known->second
                                           : child.isOver()           ? -1000.0 * gameResult(child)
                                                                      : -learnedLogit(child);
                            order.push_back({score, reply});
                        }
                    }
                    std::sort(order.begin(), order.end(), [](auto& x, auto& y) { return x.first > y.first; });
                    bool refuted = false;
                    for (int t = 0; t < maxTries && t < (int)order.size() && !refuted; t++) {
                        Position afterReply = afterMove;
                        afterReply.play(order[t].second);
                        solver.clear();
                        auto start = std::chrono::steady_clock::now();
                        // does the attacker win (avoid losing, with REFUTE_LOSS) after this reply?
                        int value = mustLose ? solver.solve(afterReply, 0, nodeBudget, -1, 0) : solver.solve(afterReply, 0, nodeBudget, 0, 1);
                        if (value == Solver::UNKNOWN || (mustLose ? value >= 0 : value >= 1)) continue;
                        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                        std::lock_guard<std::mutex> guard(lock);
                        refutations[order[t].second.placementId]++;
                        fprintf(log, "%s refuted %s %llu %.1f\n", moveToString(m).c_str(), moveToString(order[t].second).c_str(),
                                (unsigned long long)solver.nodes, seconds);
                        fflush(log);
                        refuted = true;
                    }
                    std::lock_guard<std::mutex> guard(lock);
                    status[moveToString(m)] = refuted ? "refuted" : "open";
                    if (!refuted) {
                        fprintf(log, "%s open\n", moveToString(m).c_str());
                        fflush(log);
                    }
                    printf("%zu/%zu %s %s\n", k + 1, todo.size(), moveToString(m).c_str(), refuted ? "refuted" : "open");
                    fflush(stdout);
                }
            });
        for (auto& t : pool) t.join();
    };

    // Pass 2 on every open move, the attacker's best first: all of the defender's replies, which
    // either refutes the move or proves that it survives.
    auto fullPass = [&] {
        for (auto& [score, m] : ranked) {
            std::string move = moveToString(m);
            if (!survivor.empty()) return;
            if (statusOf(move) != "open") continue;
            Position afterMove = pos;
            afterMove.play(m);
            // does the defender lose (or, with REFUTE_LOSS, fail to win)?
            RootSolveResult r = parallelSolve(afterMove, 1e9, threads, 22, {}, nullptr,
                                              mustLose ? SolveQuestion::WIN_TEST : SolveQuestion::LOSS_TEST);
            int refuting = mustLose ? 1 : 0;  // a defender reply this good refutes the move
            if (r.value != Solver::UNKNOWN && r.value < refuting) {
                status[move] = survives;
                survivor = move;
                fprintf(log, "%s %s\n", move.c_str(), survives);
            }
            for (auto& [reply, value] : r.perMove)
                if (r.value == refuting && value == refuting) {
                    status[move] = "refuted";
                    fprintf(log, "%s refuted %s\n", move.c_str(), moveToString(reply).c_str());
                    break;
                }
            fflush(log);
            printf("pass 2: %s %s\n", move.c_str(), statusOf(move).c_str());
            fflush(stdout);
        }
    };

    size_t chunk = std::max(1, envInt("REFUTE_CHUNK", 64));
    fullPass();
    for (size_t from = 0; from < todo.size() && survivor.empty(); from += chunk) {
        quickPass(from, std::min(todo.size(), from + chunk));
        fullPass();
    }
    fclose(log);

    size_t refutedCount = 0;
    for (Move m : all) refutedCount += statusOf(moveToString(m)) == "refuted";
    if (!survivor.empty()) printf("the side to move %s with %s\n", mustLose ? "does not lose" : "WINS", survivor.c_str());
    else if (refutedCount == all.size()) printf("the side to move %s: all %zu moves refuted\n", mustLose ? "LOSES" : "cannot win", all.size());
    else printf("%zu of %zu moves refuted, rest unknown\n", refutedCount, all.size());
    return 0;
}
