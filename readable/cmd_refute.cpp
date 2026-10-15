// Refutation sweeps from the command line (see refute.hpp): refute, proveline.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include "commands.hpp"
#include "notation.hpp"
#include "refute.hpp"

// The position after argv[firstMove...]; false (after printing which move) if a move is illegal.
static bool playMoves(const Args& args, int firstMove, Position& pos) {
    pos = Position::initial();
    for (int i = firstMove; i < args.argc; i++)
        if (!playMoveText(pos, args.argv[i])) {
            printf("illegal move %s\n", args.argv[i]);
            return false;
        }
    return true;
}

// ---- refute ----------------------------------------------------------------------------------

// First line of a refute log: the position, the question (REFUTE_LOSS) and SOLVER_RED_BONUS.
static std::string refuteLogHeader(const Position& pos) {
    return "# refute " + positionToString(pos) + " loss " + std::to_string(envInt("REFUTE_LOSS", 0) != 0) + " bonus " + std::to_string(RED_BONUS);
}

// snek refute <threads> <log> <move>...
// Does the side to move after the given moves win, or can each of its moves be refuted? With env
// REFUTE_LOSS=1: does it avoid losing, or can each move be refuted with a loss? REFUTE_FIRST=<file>
// lists moves (one per line) to settle before all others.
int cmdRefute(const Args& args) {
    if (!args.has(4)) { printf("usage: snek refute <threads> <log> <moves...>\n"); return 1; }
    int threads = args.arg(2, 1);
    Position pos;
    if (!playMoves(args, 4, pos)) return 1;
    std::vector<std::string> first;
    if (const char* path = getenv("REFUTE_FIRST")) {
        std::ifstream in(path);
        std::string move;
        while (in >> move) first.push_back(move);
    }
    bool mustLose = envInt("REFUTE_LOSS", 0) != 0;
    RefuteResult r = refuteMoves(pos, threads, args.arg(3, ""), refuteLogHeader(pos), first);
    if (!r.error.empty()) { printf("%s\n", r.error.c_str()); return 1; }
    if (!r.survivor.empty()) printf("the side to move %s with %s\n", mustLose ? "does not lose" : "WINS", r.survivor.c_str());
    else if (r.refuted == r.total) printf("the side to move %s: all %zu moves refuted\n", mustLose ? "LOSES" : "cannot win", r.total);
    else printf("%zu of %zu moves refuted, rest unknown\n", r.refuted, r.total);
    return 0;
}

// ---- proveline -------------------------------------------------------------------------------

// snek proveline <threads> <dir> <move>...
// Settles every reply of the side to move after the given moves (the defender) with a refutation
// sweep for the opponent (the attacker): an attacker answer that wins against all defender replies
// after it, or proof that the attacker cannot win after that reply. Answers that won against
// earlier replies are tried first (the same answer often wins again). The defender's best replies
// by the evaluation come first, so a reply that breaks the line shows up early.
//
// dir/summary.txt gets a line per settled reply, and a rerun continues after them:
//   <reply> wins <answer> <seconds>
//   <reply> holds <seconds>                      (the attacker cannot win after this reply)
// dir/<reply>.log (':' and ',' as '_') are the sweeps' resumable logs.
int cmdProveLine(const Args& args) {
    if (!args.has(4)) { printf("usage: snek proveline <threads> <dir> <moves...>\n"); return 1; }
    if (envInt("REFUTE_LOSS", 0)) { printf("proveline proves wins; REFUTE_LOSS is for refute only\n"); return 1; }
    int threads = args.arg(2, 1);
    std::string dir = args.arg(3, "");
    Position pos;
    if (!playMoves(args, 4, pos)) return 1;
    std::filesystem::create_directories(dir);
    std::string summaryPath = dir + "/summary.txt";
    std::string header = "# proveline " + positionToString(pos) + " bonus " + std::to_string(RED_BONUS);
    std::string existing = firstLineOf(summaryPath);
    if (!existing.empty() && existing != header) {
        printf("%s starts with \"%s\", not \"%s\"\n", summaryPath.c_str(), existing.c_str(), header.c_str());
        return 1;
    }
    if (existing.empty()) {
        FILE* summary = fopen(summaryPath.c_str(), "a");
        fprintf(summary, "%s\n", header.c_str());
        fclose(summary);
    }

    // what earlier runs settled, and the answers that won (most recent first)
    std::map<std::string, std::string> settled;  // defender reply -> "wins" or "holds"
    std::vector<std::string> winningAnswers;
    {
        std::ifstream in(summaryPath);
        std::string line;
        while (std::getline(in, line)) {
            std::istringstream fields(line);
            std::string reply, state, answer;
            if (line[0] == '#' || !(fields >> reply >> state)) continue;  // the header
            settled[reply] = state;
            if (state == "wins" && fields >> answer && std::find(winningAnswers.begin(), winningAnswers.end(), answer) == winningAnswers.end())
                winningAnswers.insert(winningAnswers.begin(), answer);
        }
    }

    // the defender's replies, best first by the evaluation
    std::vector<std::pair<double, Move>> replies;
    for (Move m : symmetryUniqueMoves(pos)) {
        Position child = pos;
        child.play(m);
        replies.push_back({child.isOver() ? -gameResult(child) * 1000.0 : -learnedLogit(child), m});  // the defender's view
    }
    std::stable_sort(replies.begin(), replies.end(), [](auto& a, auto& b) { return a.first > b.first; });

    size_t won = 0, held = 0;
    for (auto& [score, m] : replies) {
        std::string reply = moveToString(m);
        if (settled.count(reply)) {
            won += settled[reply] == "wins";
            held += settled[reply] == "holds";
            continue;
        }
        Position afterReply = pos;
        afterReply.play(m);
        std::string logName = reply;
        std::replace(logName.begin(), logName.end(), ':', '_');
        std::replace(logName.begin(), logName.end(), ',', '_');
        auto start = std::chrono::steady_clock::now();
        RefuteResult r = refuteMoves(afterReply, threads, dir + "/" + logName + ".log", refuteLogHeader(afterReply), winningAnswers);
        if (!r.error.empty()) { printf("%s\n", r.error.c_str()); return 1; }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        bool attackerWins = !r.survivor.empty(), attackerCannotWin = r.refuted == r.total;
        FILE* summary = fopen(summaryPath.c_str(), "a");
        if (attackerWins) {
            fprintf(summary, "%s wins %s %.0f\n", reply.c_str(), r.survivor.c_str(), seconds);
            winningAnswers.erase(std::remove(winningAnswers.begin(), winningAnswers.end(), r.survivor), winningAnswers.end());
            winningAnswers.insert(winningAnswers.begin(), r.survivor);
            won++;
        } else if (attackerCannotWin) {
            fprintf(summary, "%s holds %.0f\n", reply.c_str(), seconds);
            held++;
        }
        fclose(summary);
        std::string verdict = attackerWins ? "answer " + r.survivor : attackerCannotWin ? "the attacker cannot win" : "unknown";
        printf("[%zu/%zu] %s: %s (%.0f s; %zu won, %zu held)\n", won + held, replies.size(), reply.c_str(), verdict.c_str(), seconds, won, held);
        fflush(stdout);
    }
    printf("%zu replies: %zu with a winning answer, %zu where the attacker cannot win, %zu open\n", replies.size(), won, held,
           replies.size() - won - held);
    return 0;
}
