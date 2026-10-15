# Snekagon Lab

Engine, exact solver and a play/analysis UI for **Snekagon** (2-player abstract snake-building game).

## Quick start

```bash
# build (g++ 13, static so the exe runs without MinGW DLLs on PATH)
g++ -O3 -march=native -std=c++20 -pthread -static -o build/snek.exe src/main.cpp -lws2_32   # Windows (MinGW)
bash dist/build.sh                                                                    # Linux

# UI: http://localhost:8080  (second arg = engine threads)
node ui/server.js 8080 10
```

The browser UI only draws; every computation runs in the C++ engine (`ui/server.js` starts `snek.exe serve`
and forwards each request to its stdin, one JSON line back per command).

UI: pick a piece (buttons or keys `1`–`8`), hover a cell, and cycle through all legal placements of that
piece covering the cell with the mouse wheel / `R` / right click; click to play.
- **Engine move** – the alpha-beta engine plays (the first version, MCTS, is still there in the protocol: `engine mcts`).
- **Analyze** – ranked moves with win probability for the side to move (hover = preview, click = play).
- **Solve** – exact parallel solve of the current position (win/draw/loss per move) within a time limit, with a
  progress bar of the solved moves. Offered once few pieces are left (from move 7 in the browser, move 6 with the
  server): earlier positions can take minutes.
- **live evaluation** – every shown position (also while browsing the game) gets a 1 s analysis, cached per
  position, so going back and forth is instant; "Analyze" with a longer time replaces the cached one.
- game navigation: ⏮ ◀ ▶ ⏭, arrow keys, Home/End, or click a move in the history. The whole game is kept;
  playing a different move in an earlier position starts a new branch.
- checkboxes let the engine play either side.
- **Exact verification** – exact solve of the shown position as a separate background process (the UI stays
  responsive): progress, ETA, verdict with what it implies for the last two moves, winning/drawing moves.
  With "automatically after move 4" it starts by itself once the 4th move is played. Jobs are resumable and
  cached in `results/jobs/` (`<id>.moves`, progress `<id>.log`, output `<id>.out`).
- `?moves=1:60+4B:16,27,28,39+…` in the URL opens that position.

Server: `node ui/server.js <port> <engineThreads> <jobThreads>` (default job threads = cores − 4).

## Browser version (no server, GitHub Pages)

The same UI also runs as a static site: `src/web.cpp` compiles the engine to WebAssembly (`bash web/build.sh`,
needs Emscripten; output in `web/site/`), `ui/snek-worker.js` runs it in a Web Worker, and `ui/index.html`
switches to it whenever `ui/server.js` is not there. In the browser the engine thinks on a single core and the
exact verification jobs are hidden. `.github/workflows/pages.yml` builds it and publishes it on GitHub Pages
on every push to `main`. Both front ends share the protocol in `src/serve.hpp`.

## Rules as implemented: [RULES.md](RULES.md)

## Engine (v2)

| Component | What it does |
|---|---|
| move generation (`core.hpp`) | for each of the 40 piece orientations the legality of **all anchor cells at once** is an AND/OR of shifted 128-bit boards (no-overlap, isolation for new snakes, own-degree ≤ 2, no sharp turns, touching own). ~2.6 µs for a full move list, perft 3 in 0.95 s (4.3× faster than v1). |
| length bounds (`snakeBounds`) | **sound** lower/upper bound of each player's final longest snake: a player can never again occupy a cell next to its own snake body, so the final snake lies in one cluster of *own cells ∪ growable cells*; new cells ≤ subset-sum fit of the remaining pieces into the growable regions; with k pieces left at most the k + 1 longest snakes of a cluster can end up in one final snake (old snakes and new pieces alternate along it). Detects enclosed snakes immediately. |
| evaluation (`eval.hpp`) | features from a Tron-style Voronoi split (who reaches which cell first from its growable snake ends), per-snake territory, bounds, number of snakes…; **logistic regression fitted on ~68 000 positions whose exact result was computed by the solver** (`tools/fit_eval.py`). Hold-out: predicts the winner in 87–90 % of decisive positions (hand-made eval: 72 %). |
| search (`search.hpp`) | iterative-deepening PVS α-β, TT on symmetry-canonical keys, killers/history, forward pruning (best 24 by 1-ply eval at depth ≥ 2), bound cutoffs, the exact solver once ≤ 6 pieces remain; parallel over root moves with multi-PV. Depth 4–5 in 3 s early in the game. The default engine `ab2` adds exact verdicts on the root moves once ≤ 10 pieces remain (null-window win and draw proofs with all threads, interleaved with the search), which is where games between the engines are decided. |
| exact solver (`solver.hpp`) | negamax over {loss, draw, win}, TT, bound cutoffs, **threat zones**, an exact closed form for the last two pieces, TT/killer/static ordering + learned-eval ordering when ≥ 7 pieces remain; `solve()` takes a window, so "does the side to move win?" is a cheaper null-window question; `parallelSolve` splits root moves over threads. |
| threat zones (`Solver::scanThreats`) | an opponent placement *b* is a threat when, played as a free move, it makes the bounds decide the game. Any move that leaves *b* playable (does not cover it, nor touch it when *b* starts a new snake) lets the opponent answer with *b*, and the bounds only tighten from there, so such moves are worth at most that result: only moves through the zone of every threat are searched (the "must-play" region of Hex solvers). Threats come from the killer refutations one ply down and, once a node's first move fails, from a scan of all opponent placements, filtered cluster by cluster. Two threats with disjoint zones end the node without searching any move. |

**Strength:** `snek arena` (random opening = random 1-hex + random reply, each opening played with both
colours, 1 s/move):

| match | result |
|---|---|
| α-β with the hand-made eval vs MCTS (v1) | 2 : 10 |
| α-β with the learned eval vs MCTS (v1) | **16 : 0** (weights v1), **7 : 0, 1 draw** (current weights) |
| `ab2` vs `ab` (3 threads each, 40 openings, seed 5151) | **45 : 31, 4 draws** (58.8 %) |

`ab` throws away 30 % of the won or drawn positions at ply 6 (42 of 141 positions from 168 engine games, 1 s
per move); `ab2` 5 % (7 of 141), because from 10 pieces left the solver tells a won move from a lost one
within the move time.

The hand-made eval lost because it treated all own tiles in one region as if they would be joined into one
snake – the engine scattered pieces. The fitted weights confirm it: *number of separate snakes* has a large
negative weight, *best single snake + its own territory* and the *sound upper bound* are the strongest positive
features, and the side to move has a big tempo advantage.

### Side to move, "jumping" evaluation

The evaluation used to swing from ~90 % to ~10 % between consecutive plies. Diagnosis (`snek evaltrace`,
`snek evalexplain`):

1. **Tempo in the static evaluation.** A linear model cannot measure what a move just gained, so it carries the
   value of having the move in explicit terms (bias for the side to move, `stmIsP1`, remaining-cells difference):
   ≈ +0.6 … +1 logit for whoever is to move. Part of it is real (even in good play the side to move wins 57 %),
   part was an artefact of the first training data (random moves → the move before a position was often a
   blunder → side to move won 69 %). Data from (almost) greedy play reduced the tempo terms.
2. **Odd/even search depth.** Leaves at even and odd depth have the opposite side to move, so the root value
   flipped with the depth reached. The displayed value of the position is now the average of the best
   scores of the last two iterations.
3. **Opening out of the training range** (labels only from ply 5–6 on). Large open territories there
   make the Voronoi head start of the side to move worth dozens of cells; player 2 before its first move had
   "no snake" features (fixed). A per-ply calibration on 245 exactly solved ply-5 positions found *no*
   over-confidence at ply 5 (best scale 1.18), so no shrinking is applied (`EVAL_PLY_SCALE` = 1).
4. A real bug found on the way: forward pruning could report a **false forced win** (all searched replies
   lost, but a pruned one saved the game). Fixed.

Result on the self-play game used for diagnosis (red's win %, what the UI shows after 3 s vs exact):
ply 4 66 % (red wins), ply 5 51 % (draw; before: a false "forced blue win"), ply 6+ 100 % (red wins).
**Remaining:** in the opening (plies 1–4) the value still moves by about ±15 points depending on who is to
move – fixing that needs a richer evaluation (e.g. a small network on the board) and opening labels.

Training data now: 68k solver-labelled positions (`data/labels_*.csv`, newer files also store the position,
so `snek refeature` can recompute features without re-solving).

## Solving – measurements

`snek solvebench <plies> <n> <timeout> <threads>` solves random positions after `<plies>` random moves
(ply 0 = the 1-hex). Nodes on the **same positions**, v1 → v2 solver:

| plies played | v1 (nodes, all 12 positions) | v2 |
|---|---|---|
| 7 | 5.5 M | 0.94 M |
| 6 | > 299 M (1 timeout) | 60 M |
| 5 (8 pos.) | 1.75 G, median ~7 min | **162 M, median ~18 s** (same values) |

Positions from **good play** (`snek solvepos`, engine self-play game in `results/selfplay_*`, 6–14 threads):

| position | result | cost |
|---|---|---|
| ply 6 (red to move) | red wins | 1.6 M nodes, 0.7 s |
| ply 5 (blue to move) | **draw** – all 336 moves solved; the engine's actual move there loses | 173 M nodes, 73 s (6 threads) |
| ply 4 (red to move) | **red wins** with `4B:93,94,102,103` (of 38 moves solved: 1 win, 4 draws, 33 losses); the engine played a drawing move | 1.2 G nodes, 157 s (14 threads) |

### Centre opening: what is proven

| position | result |
|---|---|
| `1:60 4B:16,27,28,39 4A:7,8,9,20 4I:15,25,35,45` (after 4 moves, red to move) | **red loses** – all 956 moves (pilot, ~10 thread-hours) |
| `1:60 4B:16,27,28,39 4A:7,8,9,20` (after 3 moves, blue to move) | **blue wins** with `4I:15,25,35,45` |
| `1:60 4B:16,27,28,39 3V:40,41,50` (the reply that resisted 40 candidates and 282 thread-hours) | **blue wins** with `3V:9,20,21`, a move the evaluation ranks 923rd of 1032. All 1039 red replies are blue wins, by win tests in 0.65 thread-hours (`results/stubborn_3V9_wintest.log`) and again with exact values by the v3 solver of commit ed2c6b1 (`results/stubborn_3V9_ply4.*`, 10.8 thread-hours). Every other blue move is refuted by a red reply after which blue does not win (`results/stubborn_refute.log`, the `snek refute` log; 60 of these refutations drawn at random, re-solved by the v3 solver: 48 blue losses, 12 draws, `results/stubborn_refute_sample_v3.log`), so `3V:9,20,21` is blue's only winning move here |
| `1:60 4B:16,27,28,39 4C:40,41,50,52` (red's U-block: grows from the centre and wraps the head of blue's snake) | **blue cannot win**: each of blue's 938 moves is refuted by a red reply after which blue does not win (`results/hold_4C40_refute.log`, the `snek proveline` sweep), and the v3 solver of commit ed2c6b1 confirms every one with exact values: 824 blue losses, 114 draws (`results/hold_4C40_refute_v3.log`). So blue's `4B:16,27,28,39` does **not** win against the centre. Blue holds the draw with `4A:49,59,70,81` (`REFUTE_LOSS=1` sweep, `results/hold_4C40_draw_refute.log`; the v3 solver gives value 0 after it: of red's 635 replies 633 lose and 2 draw, `results/hold_4C40_4A49_ply4.*`), so this position is a **draw** |
| `1:60 4B:16,27,28,39 3V:93,94,103` | **blue wins** with `4A:49,59,70,81` (670 red replies, `results/reply_3V93_4A49_wintest.log`); the 6 best blue moves by a 3 s search do not win |
| `1:60 4B:16,27,28,39 4I:102,103,104,105` | **blue wins** with `4C:64,75,84,85`, about the 440th of 872 blue moves in the evaluation's order (`results/reply_4I102_refute.log`) |
| `1:60 4B:16,27,28,39 3V:101,102,113` | **blue wins** with `4I:15,25,35,45` (822 red replies) |
| `1:60 4B:16,27,28,39 2:93,94` | **blue wins** with `4I:40,51,62,73` (751 red replies) |
| `1:60 4B:16,27,28,39 3V:29,40,50` | **blue wins** with `4A:6,7,8,19` (all 1090 red replies in 0.65 thread-hours; 241 in the distributed run, which included finding the answer) |

### Solver v3 (speed-up round)

Benchmark `bench/ply5_blue_wins.txt` = 24 sub-positions of the pilot (ply 5, blue to move and winning),
`snek benchset <file> <threads>`:

| version | thread-seconds | wall, 16 threads |
|---|---|---|
| v2 | 510 | 61 s |
| + 2-way lockless TT (work-preferred slot) | 478 | |
| + incremental snakes in `Pos` (no flood fills for lengths), bounds that flood only the free regions, symmetric hashing only up to ply 6 | **274** | **35 s** |

Tried and rejected (measured, no gain): enhanced transposition cutoffs, a TT shared by all threads, deeper
learned-eval ordering (fewer nodes but the eval costs more than it saves), eval ordering only for the side
that looks winning, a cheap single-region bound before the exact one, and "inert move" equivalence (a
provably safe definition covers well under 1 % of moves). Profiling (`PROFILE=1 snek benchset …`) shows
that the winning side finds a winning move at the first try ~90 % of the time; the cost is the loser having
to answer every move. That first winning move, however, rarely has the smallest proof (next sections).

### Threat zones, the last two pieces, win tests

The loser's "every move" is where threat zones cut: once a node's first move fails, the opponent's free
placements that would make the bounds decide restrict the moves left to search (at red's nodes in these units,
a third to two thirds of the moves are skipped), and two threats with disjoint zones end the node at once.
Below them, the last two pieces are a closed form instead of two plies of search. Same benchmark, one fresh
process per unit (empty tables), 4 at a time on a Ryzen 7 250; each row adds one change:

| solver | nodes | thread-seconds |
|---|---|---|
| v3 (commit ed2c6b1) | 361.2 M | 157.4 |
| + threat zones | 120.4 M | 72.2 |
| + the last two pieces in closed form | 57.0 M | 49.6 |
| + at most k + 1 old snakes in a final snake | 46.9 M | 45.8 |
| + cheaper threat scans, one sort at all-nodes | 48.1 M | 41.1 |
| + no history heuristic | 28.6 M | 24.5 |
| + evaluation ordering only after the table or killer move fails | 28.6 M | 23.5 |
| win test only (`BENCH_WINTEST=1`, what a unit needs) | 25.4 M | 20.9 |
| win test with `PnSearch` (`BENCH_PNS=1`, next section) | **12.8 M** | **17.2** |

On 28 units sampled like the distributed solve's (random red replies to `1:60 4B:16,27,28,39`, blue's answer
from a 2 s search, a random red move; and red's moves after the hardest proven reply, `3V:29,40,50
4A:6,7,8,19`): 351.8 M nodes and 151.7 thread-seconds with v3, 30.5 M and 29.5 with the win test. Units where
blue does not win gain as much: the two lines after the stubborn reply that the video shows (blue's U-block
and blue's `4A:58,69,80,81`) take 56.1 M nodes and 42.8 s with v3, 5.5 M and 8.4 s with the win test. Every
change keeps the values: each one was checked on 1440 random positions after 8–12 plies
(96 per ply and red bonus −1/0/+1, so that draws and losses occur) against the solver without it.

Tried and rejected (measured): keeping the table between the units of one proof (no transpositions worth
it; with the history scores kept as well, 4x more nodes), aged or scaled history, a growth bonus in the
static ordering, evaluating only the best 4-8 moves one level deeper, and a tighter bound that measures the
shortest gap between snake ends and the best chain of snakes the remaining cells can join (7 % fewer nodes,
slower).

### Win test: cheapest proofs first (`pns.hpp`)

A distributed unit only asks whether blue wins (`SOLVER_WIN_TEST=1` workers ask just that). `PnSearch`
(`BENCH_WINTEST=1 BENCH_PNS=1`) answers the same question but looks for cheap proofs first:

- **Ordering of its alpha-beta probes**: killers behind the learned-eval ordering
  (`SOLVER_KILLERS_OVER_EVAL=0`, the default inside `PnSearch`): a move that made cutoffs in one subtree
  usually still wins in the next, but often with a much bigger proof than other winning moves. The plain
  solver keeps the killers ahead and evaluates the other moves only when they fail, which is faster in time
  over all unit sets (fewer evaluations) although it visits a few more nodes.
- **Proof-cost ordering at the root.** In the pilot's units, 10-32 of the eval's 40 best blue moves win,
  and together the cheapest proofs are 2.4x cheaper than the ones the win test commits to (up to 9x in a
  unit). A move's proof is the sum over red's 200-500 replies, and the typical reply cost differs ~10x
  between moves where the eval sees no difference. So the eval's 8 best moves are screened: a sample of
  each move's replies, spread over the eval order, gets alpha-beta probes of 1000 nodes, and
  replies x the fraction of the sample above that budget estimates the move's proof number (its number of
  hard replies). Rounds of 8, 8 and 16 samples halve the field each time (successive halving); a sampled
  reply that is no blue win drops the move. The moves are then proved in the order of the estimate.

Every answer comes from exact alpha-beta win tests, so budgets and samples only decide which proof is
tried first. Win tests with fresh tables per unit (`BENCH_FRESH=1`), 4 at a time:

| set, 24 units each (`bench/`) | win test: nodes, thread-s | `PnSearch`: nodes, thread-s |
|---|---|---|
| `ply5_blue_wins.txt` (pilot) | 25.4 M, 33.0 | **12.8 M, 23.0** |
| `ply5_blue_wins_holdout.txt` (pilot) | 20.6 M, 33.9 | **11.8 M, 24.3** |
| `ply5_random_units.txt` | 34.6 M, 47.5 | 32.2 M, 49.5 |
| `ply5_other_openings.txt` | **26.1 M, 35.2** | 27.9 M, 48.6 |

The pilot sets are red's moves in the pilot position (the held-out set: 24 others in the benchmark's cost
range, a fixed random draw, not used for tuning). The random units are drawn like the distributed solve's:
a random red reply to `1:60 4B:16,27,28,39`, blue's answer from a 2 s search, a random red move. The other
openings are blue wins from two earlier verification jobs (`results/jobs`). The screening pays off where
many blue moves win with very different proof costs, as in the pilot; elsewhere fewer moves win, the cheap
winners often rank below the top 8 and the screening costs more than it saves, so workers use the plain
win test. Per unit the sample matters (0.5-2x between seeds, `PNS_SEED`).

Tried and rejected (measured): classic best-first proof-number search over the first 2-3 plies, leaves
initialised from the eval and typical costs and probed with doubling budgets (1.8-7x more nodes than
alpha-beta); screening the expensive ply-7 nodes the same way, or racing blue's best answers there under
doubling budgets and trying race winners first at the sibling nodes (large gains in a few units, losses in
others, neutral in total, although fresh single-node tests show 3x cheaper answers exist there); ordering
by the defender's mobility after the move (2-4x more nodes: the low-mobility move often does not win);
12 or 16 screened moves, sample budgets of 500 or 2000, other sample sizes (none more than 5 % better);
switching to the next screened move when the chosen one turns out expensive (simulated, no gain).

### Is a full solution feasible?

A proof that blue wins the game is, for each of red's 12 opening cells, one blue reply, and for every red reply
to that, one blue answer after which every red move is a unit that blue wins. After the engine's preferred blue
reply (4 s search), the openings leave 608-1589 red replies each, 15 259 in all; the centre leaves the most:

| red's 1-hex | 5 | 6 | 7 | 16 | 17 | 18 | 27 | 28 | 38 | 39 | 49 | 60 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| red replies after blue's preferred reply | 1338 | 1329 | 1286 | 1240 | 1195 | 1217 | 1289 | 1316 | 1481 | 608 | 1371 | 1589 |
| engine's chance for blue | 75 % | 77 % | 71 % | 63 % | 67 % | 61 % | 62 % | 70 % | 71 % | 83 % | 78 % | 59 % |

Settling one red reply with `snek refute` (the winning answer and its proof) took, for ten replies to
`1:60 4B:16,27,28,39` drawn at random, 0.06-3.7 thread-hours: 0.5-3.4 (mean 1.5) for the first four, before
answers that won against earlier replies were tried first, and 0.06-3.7 (mean 0.84, median 0.27) for the next
six, five of which fell to such an answer. Hard replies are the ones whose only winners rank deep in the
evaluation's order (the stubborn `3V:40,41,50`: about 5 thread-hours, of which 4.3 to refute 1017 other moves).
So, if blue wins, the centre line is about 1589 × (0.84-1.5) ≈ **1.3-2.4·10³ thread-hours** (3-6 days of a
16-thread laptop, `snek proveline`), and the whole game about **1.3-2.3·10⁴ thread-hours** (1-2 months of the
laptop, about a day on 1000 cores), plus finding blue's reply to each opening. A drawn game needs strategies for
both sides and would cost a few times more. These are estimates from samples, not bounds.

## Distributed solve of the centre opening (`dist/`)

Proves (or refutes) that after `1:60 4B:16,27,28,39` blue wins: for each of red's 1589 replies the
coordinator tries blue candidates and sends every red answer as a unit (position after 5 moves) to the
workers. A unit that does not come back as a blue win discards the candidate.

- blue's candidates are ranked by a 10 s alpha-beta search (precomputed in the background); the 1-ply
  evaluation alone proposed near-identical moves that all failed against the same red answer,
- red answers that refuted earlier candidates ("killers") are tried first for the next one,
- units carry a node budget (`PROBE_NODES`, default 150 M); over-budget units are solved later without
  budget, an over-budget killer marks the candidate suspect (retried only if nothing else works).
  A candidate is accepted only when every unit is an exact blue win, so budgets never affect soundness.

Updating a running installation: `bash dist/stop.sh`, unzip the new version over the old folder
(`results/dist/` is kept), `bash dist/build.sh`, `bash dist/start.sh` - old state is migrated (killers are
rebuilt from `results.log`).

On a Linux box (runs nonstop, coordinator + local worker):

```bash
sudo apt install build-essential python3
bash dist/start.sh          # builds build/snek, starts coordinator + worker, prints the status URL
bash dist/stop.sh           # stop (state in results/dist/ is kept, start.sh resumes)
```

Other machines on the same network join with `snek worker <ip>:8090 <threads>` (Windows:
`snek.exe worker …`). Status page: `http://<ip>:8091/` (also `/events`, `/state`). Earlier verification results from
`results/jobs/` are reused automatically. Workers started with `SOLVER_WIN_TEST=1` only ask whether blue wins each
unit (about 20% less work); a unit that is not a blue win then comes back as 0 (draw or loss) or -1 (loss).

## Tools

| Command | Purpose |
|---|---|
| `snek serve [threads]` | UI protocol (below) |
| `snek arena <pairs> <sec> <threads> <A> <B> [seed]` | engine match; engines `ab`, `abhand`, `mcts`, `ab:<weights.txt>` |
| `snek gendata <games> <plyMin> <plyMax> <solveSec> <threads> <seed>` | labelled positions (features + exact value) as CSV |
| `python tools/fit_eval.py data/*.csv` | fit the evaluation → `src/eval_weights.hpp`, `data/weights_latest.txt` |
| `snek solvepos <sec> <threads> <moves…>` | parallel exact solve of a position (`PROGRESS=<file>` logs and resumes; `SOLVER_WIN_TEST=1` only proves or refutes that the side to move loses: each root move is `loss` or `notloss`, much cheaper) |
| `snek refute <threads> <log> <moves…>` | shows that the side to move cannot win (`REFUTE_LOSS=1`: that it loses), or finds a move that survives: for every move a reply after which the mover does not win (replies that refuted other moves first, then by the evaluation; `REFUTE_TRIES` × `REFUTE_NODES` M nodes), then a full search over all replies for the moves left open, in chunks of `REFUTE_CHUNK` moves; `REFUTE_FIRST=<file>` settles the listed moves first (answers that won against sibling replies often win again); the moves are taken in the evaluation's order after the 8 best of a `REFUTE_SEARCH` (2) second engine search, which ranks easy winners far higher. Resumable log, headed by the position, `REFUTE_LOSS` and `SOLVER_RED_BONUS` (a log for anything else is refused); every refutation is one position anyone can re-solve |
| `snek proveline <threads> <dir> <moves…>` | settles every reply of the side to move with `refute` for the opponent: an answer that wins against all replies after it, or proof that the opponent cannot win there. Answers that won against earlier replies are tried first; `dir/summary.txt` (one line per reply) and the sweeps' logs make it resumable, so one machine can work through a whole line. It stops at the first reply after which the opponent cannot win, which settles the line (`PROVELINE_ALL=1` goes on), e.g. `snek proveline 16 centre 1:60 4B:16,27,28,39` |
| `snek solvebench <plies> <n> <sec> <threads>` | solver benchmark on random positions (`SOLVER_PNS=1`: values from two `PnSearch` win tests) |
| `snek selfplay <games> <sec> <threads>` | engine vs itself, prints the game |
| `snek verify [games]` | fast move generator == reference checker, incremental snakes and fast bounds == reference, bounds never violated |
| `snek benchset <file> <threads>` | solver benchmark on a fixed position set (`PROFILE=1` prints per-ply statistics, `BENCH_FRESH=1` starts every position from empty tables for reproducible node counts, `BENCH_WINTEST=1` only asks whether the side to move wins, `BENCH_PNS=1` with it uses `PnSearch`; a build with `-DSOLVER_TIMING` adds exclusive time per ply and per step) |
| `snek perft <d>`, `snek speed`, `snek info` | counting / micro-benchmarks |
| `python ref/crosscheck.py [games]` | engine vs independent Python rules implementation |

### Engine protocol (`snek serve [threads]`, one line in → one JSON line out)

- `new`, `state`, `undo`, `set <m1> <m2> …` (position from the start), `play <m> …` (append moves)
- move notation: `pass` or `<piece>:<cell>,<cell>,…`, cell = `(r+5)*11 + (q+5)` (axial q,r; centre = 60).
  Pieces: `1 2 3I 3V 4I 4A 4B 4C`.
- `analyze <sec>` / `go <sec>` – ranked moves (score = 100·logit of the win probability, ±100000 = proven),
  `go` also plays the best move
- `solve <sec>` – parallel exact solve
- `engine ab|mcts`

## Verification

- Placement counts match the independent Python enumeration (2602 placements).
- `ref/crosscheck.py`: legal-move sets and snake lengths identical to the Python reference (398 positions).
- `snek verify 3000`: bit-parallel generator gives the identical move list (same order) as the per-placement
  checker in 47 131 positions; the length bounds held for the final result in all 94 262 checks.
- Solver v2 gives the same values as v1 on all benchmark positions.
- `PnSearch` gives the solver's values (`SOLVER_PNS=1 snek solvebench …`, red bonus −1/0/1) on 1 608 random
  positions after 5–12 plies (screening from 8 pieces left at plies 6–7 and from 4 at plies 8–12
  (`PNS_SCREEN_REM`), so that it runs at every level), and agrees with the solver on the 96 units of the
  four `bench/ply5_*` sets (all blue wins) with every sample seed.
- `perft`: 1 → 91, 2 → 194 388, 3 → 315 706 848.

## Layout

| Path | What |
|---|---|
| `src/core.hpp` | geometry, pieces, bit-parallel move generation, bounds, notation |
| `src/eval.hpp`, `src/eval_weights.hpp` | features, learned + hand-made evaluation |
| `src/search.hpp` | α-β playing engine |
| `src/solver.hpp` | exact solver, parallel solve |
| `src/pns.hpp` | win test that screens the root moves for the cheapest proof |
| `src/mcts.hpp` | v1 MCTS engine (kept for comparison) |
| `src/main.cpp` | CLI + UI protocol |
| `readable/` | the same engine refactored for reading (identical results), with a reading guide: [readable/README.md](readable/README.md) |
| `ref/` | independent Python reference implementation + cross-check |
| `tools/fit_eval.py` | evaluation fitting |
| `data/` | solver-labelled positions (`labels_v*.csv`), weight files |
| `ui/` | Node server + single-page UI |
| `results/` | benchmark / arena / solve logs |
