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
| search (`search.hpp`) | iterative-deepening PVS α-β, TT on symmetry-canonical keys, killers/history, forward pruning (best 24 by 1-ply eval at depth ≥ 2), bound cutoffs, the exact solver once ≤ 6 pieces remain; parallel over root moves with multi-PV. Depth 4–5 in 3 s early in the game. `ab2` adds exact verdicts on the root moves once ≤ 10 pieces remain (null-window win and draw proofs with all threads, interleaved with the search), which is where games between the engines are decided. |
| network evaluation (`nn.hpp`) | the native build's default engine `nn` is `ab2` evaluating by the mean of the logistic evaluation and a small efficiently updatable network (NNUE), trained on self-play: see [Network evaluation](#network-evaluation-nn). The WebAssembly build and the UI use `ab2`. |
| exact solver (`solver.hpp`) | negamax over {loss, draw, win}, TT, bound cutoffs, **threat zones**, an exact closed form for the last two pieces, **tight bounds** near the end (the one-player optimum and guaranteed growth), TT/killer/static ordering + learned-eval ordering when ≥ 7 pieces remain; `solve()` takes a window, so "does the side to move win?" is a cheaper null-window question; `parallelSolve` splits root moves over threads. |
| one-player optimum (`OnePlayer`, `core.hpp`) | could a player still make a snake of *k* cells if the opponent never moved again? A depth-first search over induced paths and rings of whole old snakes and whole new pieces, each piece attached at an end of the path built so far, cluster by cluster with a memo. It drops the opponent's future cells, the move order and the touch-nothing rule, so a "no" is a sound upper bound. It is much tighter than the cluster bound because it follows how the fixed piece shapes chain. |
| tight bounds (`Solver::tightenBounds`) | lower bounds one and two plies ahead: the side to move's best immediate length, and the length the opponent reaches whatever single move the side to move makes (no move covers, or for a new snake touches, all its extensions). Then only the one-player questions whose answer ends the node in its window are asked, and the result goes into the table. Used at every node once ≤ 5 pieces are left in total, and with 6 left once a node's first move failed, over the moves still to search. Killer threats use the optimum as well. |
| threat zones (`Solver::scanThreats`) | an opponent placement *b* is a threat when, played as a free move, it makes the bounds decide the game. Any move that leaves *b* playable (does not cover it, nor touch it when *b* starts a new snake) lets the opponent answer with *b*, and the bounds only tighten from there, so such moves are worth at most that result: only moves through the zone of every threat are searched (the "must-play" region of Hex solvers). Threats come from the killer refutations one ply down and, once a node's first move fails, from a scan of all opponent placements, filtered cluster by cluster. A move must meet every such zone to be searched, so a few threats with small zones leave few moves. |

**Strength:** `snek arena` (random opening = random 1-hex + random reply, each opening played with both
colours, 1 s/move):

| match | result |
|---|---|
| α-β with the hand-made eval vs MCTS (v1) | 2 : 10 |
| α-β with the learned eval vs MCTS (v1) | **16 : 0** (weights v1), **7 : 0, 1 draw** (current weights) |
| `ab2` vs `ab` (3 threads each, 40 openings, seed 5151) | **45 : 31, 4 draws** (58.8 %) |
| `nn` vs `ab2` (one thread each, balanced openings, below) | **+48 Elo** at 0.3 s/move, **+10** at 1 s, **+23** at 3 s |

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
**Remaining:** in the opening (plies 1–4) the logistic evaluation's value still moves by about ±15 points
depending on who is to move. Exact labels there are too expensive (a fifth of ply-4 positions take over 5
minutes), so the network below learns plies 2–4 from search scores and game results.

Training data now: 68k solver-labelled positions (`data/labels_*.csv`, newer files also store the position,
so `snek refeature` can recompute features without re-solving).

### Network evaluation (`nn`)

`nn` is `ab2` with another evaluation: the mean of the logistic evaluation and a small network (`src/nn.hpp`;
weights `data/nn/nn_g2.bin`, compiled in through `src/nn_net.hpp`).

- **Network**: a perspective NNUE. 937 inputs: four 91-cell planes per player (snake body, other own cells,
  empty cells next to an own end, empty cells next to the body, which the player can never use), the empty
  cells next to no tile, the pieces left, and one-hot each player's longest snake and `snakeBounds` upper bound.
  They feed 128 int16 units (AVX2, with an identical scalar fallback), then SCReLU and one output per ply: the
  logit of P(side to move wins), draws ½.
- **Incremental**: the search keeps an accumulator pair per ply and updates it from the parent by the inputs a
  move changes (27 on average, at most 43), so a child costs about 0.6 µs against 1.1–1.3 µs for playing it and
  running the logistic evaluation. A leaf that the network puts more than 1 logit below alpha keeps the
  network's value. `nn` therefore searches `ab2`'s tree at `ab2`'s node rate.
- **Why a mean**: as the only evaluation the network loses to `ab2` (−46 Elo [−73, −20], 480 games at 0.3 s):
  the search picks the leaves it overrates most, and its errors on unusual positions are large. Averaged with
  the logistic evaluation, whose errors differ, it wins. `NN_EVAL=network` selects the network alone.
- **Data** (`snek datagen`): self-play from a random 1-hex and, half the time, a random reply, with random or
  sampled moves at plies 1–5. Each position is labelled with the engine's proof where it found one, else an
  exact solve under a time limit at ply ≥ 6 and in 10 % of the games at ply 5; a solve that runs out of time is
  kept as censored, never dropped. Sibling moves and positions from the search tree are added for a ranking loss.
- **Training** (`tools/train_nn.py`, numpy): the exact value where known, else ½ · sigmoid(search score) + ½ ·
  game result; a RankNet loss over siblings; one of the 12 hex symmetries per batch; games whose id hashes to 0
  mod 10 held out. `nn_g2`: 725 k positions from 11.8 k games, 56 % exactly labelled. A further generation from
  `nn` self-play gave no gain (−1 Elo [−26, +23], 480 games head to head).

Strength against `ab2` (`snek arena`, one thread each, balanced 2-ply openings from `snek book`, each played
with both colours, pentanomial 95 % intervals from `tools/arena_stats.py`):

| move time | games | `nn` vs `ab2` |
|---|---|---|
| 0.3 s | 840, fresh openings | +48 Elo [+30, +67] |
| 0.3 s, with the tight-bound solver (both engines) | 840 | +22 Elo [+4, +41] |
| 1 s | 5 880, on openings not used to tune the engine | +10 Elo [+4, +17] |
| 3 s | 840 | +23 Elo [+5, +42] |

About 70 % of the pairs split 1–1 by colour: the opening still decides most games. Offline, on held-out games
and exact labels, the network's log-loss beats the logistic evaluation's clearly at plies 7–11 (0.225 vs
0.291); at plies 5–6 they are level (0.210 vs 0.216) and at ply 5 the network is worse; the mean that `nn`
uses is better at both (0.199 at plies 5–6). On 400 held-out won ply-6 positions (`snek firstwin`) every
evaluation ranks the first winning move first about equally often (0.59–0.62); the mean ranks it higher than
the logistic evaluation in 98 positions and lower in 48. Ordering the exact solver by the network costs 1.29×
the nodes on `bench/` (values identical), so the solver keeps the logistic order.

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
| `1:60 4B:16,27,28,39 3V:40,41,50` (the reply that resisted 40 candidates and 282 thread-hours) | **blue wins** with `3V:9,20,21`, a move the evaluation ranks 923rd of 1032. All 1039 red replies are blue wins, by win tests in 0.65 thread-hours (`results/stubborn_3V9_wintest.log`) and again with exact values by the v3 solver of commit ed2c6b1 (`results/stubborn_3V9_ply4.*`, 10.8 thread-hours). Every other blue move is refuted by a red reply after which blue does not win (`results/stubborn_refute.log`, from the experimental sweep that became `snek refute`; all 1031 refutations re-run as fresh win tests, none a blue win, `results/stubborn_refute_wintest.log`; 60 of them drawn at random, re-solved by the v3 solver: 48 blue losses, 12 draws, `results/stubborn_refute_sample_v3.log`), so `3V:9,20,21` is blue's only winning move here |
| `1:60 4B:16,27,28,39 4C:40,41,50,52` (red's U-block: grows from the centre and wraps the head of blue's snake) | **blue cannot win**: each of blue's 938 moves is refuted by a red reply after which blue does not win (`results/hold_4C40_refute.log`, the `snek proveline` sweep), and the v3 solver of commit ed2c6b1 confirms every one with exact values: 824 blue losses, 114 draws (`results/hold_4C40_refute_v3.log`). So blue's `4B:16,27,28,39` does **not** win against the centre. Blue holds the draw with `4A:49,59,70,81` (`REFUTE_LOSS=1` sweep, `results/hold_4C40_draw_refute.log`; the v3 solver gives value 0 after it: of red's 635 replies 633 lose and 2 draw, `results/hold_4C40_4A49_ply4.*`), so this position is a **draw** |
| `1:60 4A:51,52,63,74 3V:29,40,50` (blue's third choice against the centre by the engine, then red's 17th reply in the evaluation's order) | **blue cannot win**: each of blue's 867 moves is refuted (`results/hold_4A51_3V29_refute.log`), every one confirmed with exact values by the v3 solver: 802 blue losses, 65 draws (`results/hold_4A51_3V29_refute_v3.log`). So blue's `4A:51,52,63,74` does **not** win against the centre either. Blue holds the draw with `4C:59,61,70,71` (`results/hold_4A51_3V29_draw_refute.log`; the v3 solver gives value 0 after it, `results/hold_4A51_3V29_4C59_ply4.*`), so this position is a **draw**; red's first 16 replies are blue wins (`results/centre_4A51_proveline.txt`) |
| `1:60 3V:36,37,48 4A:58,59,68,78` (blue's second choice against the centre by the engine, then red's 20th reply in the evaluation's order) | **blue cannot win**: each of blue's 1004 moves is refuted (`results/hold_3V36_4A58_refute.log`); 661 of these refutations, taken in random order, re-solved by the v3 solver: 595 blue losses, 66 draws, none a blue win; the rest are not re-solved (`results/hold_3V36_4A58_refute_v3.log`). So blue's `3V:36,37,48` does **not** win against the centre either. Blue holds the draw with `4C:49,50,61,71` (`results/hold_3V36_4A58_draw_refute.log`; the v3 solver gives value 0 after it: 652 of red's 653 replies lose and one draws, `results/hold_3V36_4A58_4C49_ply4.*`), so this position is a **draw**; red's first 19 replies are blue wins (`results/centre_3V36_proveline.txt`) |
| `1:60 4A:18,19,30,41 4A:42,50,51,52`, `1:60 3V:55,56,67 4C:71,73,82,83`, `1:60 3V:35,36,47 4A:18,28,38,49` (weak blue replies drawn at random from the 151 the engine rates lowest, each with red's best reply by a 5 s search) | **blue cannot win** in each: all 908, 865 and 1017 blue moves are refuted (`results/hold_4A18_4A42_refute.log`, `hold_3V55_4C71_refute.log`, `hold_3V35_4A18_refute.log`) |
| `1:60 4C:26,36,47,48 4A:58,59,68,78` (one of blue's 18 strongest replies by the engine) | **blue cannot win**: all 1057 blue moves are refuted (`results/hold_4C26_4A58_refute.log`). It is the same red reply that holds against `3V:36,37,48`. Red's first reply by a 5 s search, `4B:28,29,40,41`, is a blue win with `4A:5,6,7,15` (`results/centre_4C26_line.txt`) |
| `1:60 4B:16,27,28,39` and 40 of red's replies | each **blue wins** (`results/centre_4B_proveline.txt`, the `snek proveline` summary: red's replies in the evaluation's order, the winning answer of each; red's 30th reply in that order is the U-block above) |
| `1:60 4B:16,27,28,39 3V:93,94,103` | **blue wins** with `4A:49,59,70,81` (670 red replies, `results/reply_3V93_4A49_wintest.log`); the 6 best blue moves by a 3 s search do not win |
| `1:60 4B:16,27,28,39 4I:102,103,104,105` | **blue wins** with `4C:64,75,84,85`, about the 440th of 872 blue moves in the evaluation's order (`results/reply_4I102_refute.log`) |
| `1:60 4B:16,27,28,39 3V:101,102,113` | **blue wins** with `4I:15,25,35,45` (822 red replies) |
| `1:60 4B:16,27,28,39 2:93,94` | **blue wins** with `4I:40,51,62,73` (751 red replies) |
| `1:60 4B:16,27,28,39 3V:29,40,50` | **blue wins** with `4A:6,7,8,19` (all 1090 red replies in 0.65 thread-hours; 241 in the distributed run, which included finding the answer) |

38 refutations from the four new holds, drawn at random, re-solved by the v3 solver: 35 blue losses, 3 draws
(`results/new_proofs_sample_v3.log`).

### Other openings: blue replies that lose

| position (blue to move) | result |
|---|---|
| `1:5 4A:50,61,71,81 4B:56,57,68,69` | **blue loses**: each of blue's 744 moves is refuted by a red reply after which blue loses (`REFUTE_LOSS=1` sweep, `results/loss_1_5_4A50_4B56_refute.log`) |
| `1:5 4A:61,72,82,92 4B:46,57,58,69` | **blue loses** (671 moves, `results/loss_1_5_4A61_4B46_refute.log`) |
| `1:17 4I:69,70,71,72 4I:91,92,93,94` | **blue loses** (881 moves, `results/loss_1_17_4I69_4I91_refute.log`) |
| `1:18 4B:61,62,70,71 4I:91,92,93,94` | **blue loses** (784 moves, `results/loss_1_18_4B61_4I91_refute.log`) |

Blue's replies are ones an engine search prefers (two different runs in 1:5), and red's reply is one of red's
five best by a 5 s search. 32 of these refutations, drawn at random, re-solved by the v3 solver: all blue
losses (`results/new_proofs_sample_v3.log`). So blue needs other replies in openings 1:5, 1:17 and 1:18, and if
every blue reply loses in one of them, red wins the game. In 1:16 the same test is open with 296 of 953 blue
moves refuted and none surviving; in 1:6, 1:7 and 1:27 blue's reply held against the red reply tested.

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
a third to two thirds of the moves are skipped).
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
  each move's replies, spread over the eval order, gets alpha-beta probes of 300 nodes, and
  replies x the fraction of the sample above that budget estimates the move's proof number (its number of
  hard replies). Rounds of 8, 8 and 16 samples halve the field each time (successive halving); a sampled
  reply that is no blue win drops the move. The moves are then proved in the order of the estimate.

Every answer comes from exact alpha-beta win tests, so budgets and samples only decide which proof is
tried first. Win tests with fresh tables per unit (`BENCH_FRESH=1`), 4 at a time, before the tight bounds
(next section) and with 1000-node probes:

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

### Tight bounds near the end

The cluster bound counts cells. Near the end it often allows a length that the fixed piece shapes cannot chain
into. The solver asks one-player questions only where the cluster bound does not decide, and on the bench sets
30–58 % of them come back "no". A question costs about 4–10 k cycles (6–23 search nodes), so the tight bounds
are used at every node once at most 5 pieces are left, and with 6 left only at a node whose first move failed,
where every remaining move would have to be searched; elsewhere they cost more than they save.

Win tests with fresh tables per unit (`BENCH_FRESH=1 BENCH_WINTEST=1 snek benchset <set> 2`), without and with
the tight bounds, back to back on a shared Ryzen 7 250:

| set (`bench/`) | win test: nodes, thread-s | with tight bounds | `PnSearch` | with tight bounds |
|---|---|---|---|---|
| `ply5_blue_wins.txt` | 25.4 M, 32–34 | **3.85 M, 15–17** | 12.8 M, 22 | 3.14 M, 15 |
| `ply5_blue_wins_holdout.txt` | 20.6 M, 31–32 | **3.69 M, 16–17** | 11.8 M, 22 | 2.79 M, 15 |
| `ply5_random_units.txt` | 34.6 M, 43–48 | **4.59 M, 17–19** | 32.2 M, 44 | 5.38 M, 28 |
| `ply5_other_openings.txt` | 26.1 M, 29–34 | **6.42 M, 16–17** | 27.9 M, 42 | 6.71 M, 26 |

The same on real proof steps (2–3 threads):

| units | without | with tight bounds |
|---|---|---|
| 30 refutations of `results/hold_4C40_refute.log` (every 31st; blue does not win) | 127.1 M nodes, 178 thread-s | 23.9 M, 82 |
| 20 refutations of `results/hold_4A51_3V29_refute.log` (every 43rd) | 61.8 M, 99 | 11.5 M, 48 |
| 20 refutations of `results/hold_3V36_4A58_refute.log` (every 50th) | 116.1 M, 170 | 19.8 M, 74 |
| 20 units of `results/stubborn_3V9_wintest.log` (every 52nd; blue wins) | 23.3 M, 28 | 3.49 M, 13 |
| `snek solvebench 5 12 3600 1` (full window) | 81.6 M, 119 s | 15.5 M, 59 s |
| `snek solvebench 6 12 3600 1` | 11.6 M, 15 s | 2.47 M, 9 s |

With the tight bounds the plain win test is about as fast as `PnSearch` on the pilot sets and faster on the
others: the screening costs as much as before while the proofs got cheaper. Its probe budget is counted in
nodes, so it went down with them to 300. `REFUTE_NODES` and `PROBE_NODES` are unchanged: units need 4–7x fewer
nodes, so a cap covers more of a proof.

Tried and rejected (measured): a bound on the cells still reachable from the path's ends inside the optimum's
search (it ended 21 % of the search nodes but took 40 % of the time); the tight bounds at every node with 6 or 7
pieces left (9–17 % slower on the bench sets, although 1.4x faster on refutations); the growth bounds alone
above the gate; the optimum above the gate only where the cluster bound is within 2–4 cells of deciding; other
memo sizes. Probes of the other ideas from Hex and Mortal Coil solvers found little: a "captured cells" lower
bound adds 0.4 % of cutoffs, independent regions (sums of games) occur at 0.1–0.5 % of nodes and do not apply
anyway (the score is a maximum, and the regions share one piece supply), and parity arguments fail because the
cell graph has triangles.

### QBF and SAT

A QBF encoding of Snekagon positions (an experiment outside the repository; DepQBF and Bloqqer) gave the
solver's value on every one of 630 "win" and "not lose" queries over 170 self-play positions with 1–8
placements left. Its key is a corrective decoding: every move code stands for the chosen placement if it is
legal, else for the first legal one in a fixed order, else a pass, so the formula needs no legality reasoning
(the textbook encoding, where an illegal move loses, solved 0 of 4 positions with 6 placements left in 60 s).

| placements left | Bloqqer + DepQBF solved (300 s) | median | solver's null-window test |
|---|---|---|---|
| ≤ 5 | 100 % | ≤ 2.3 s | ≤ 0.03 ms |
| 6 | 100 % | 7.5 s | 0.16 ms |
| 7 | 90 % | 60 s | 0.64 ms |
| 8 | 5 % | > 300 s | 4.8 ms |

The gap grows 10–40x per placement and the game has 14 after the 1-hex, so QBF is no route to a solution, as
in Hex, where QBF solvers reach 7x7 puzzles. A SAT encoding of the one-player optimum (CaDiCaL) is exact, against
a real-rules brute force on 9 382 player-rows, once a snake of new pieces only must contain a piece touching no
opponent tile. `OnePlayer` gives the value without that condition (one cell more on 4 rows, as a relaxation
may) at 2–13 µs a question against 1–100 ms.

### Is a full solution feasible?

A weak solution (the value of the start and a strategy for each side that secures it) fits in days of rented
compute. A strong one does not: a Knuth random-probe estimate gives 6.5·10²⁹ games and about 10¹³ positions
after five placements. If the game is a draw, the proof has two halves (counts up to symmetry):

- **Red's half, "blue cannot win"**: red opens in the centre, which has 172 distinct blue replies (979–2304 for
  every other opening). Each needs a *line*: a red reply after which `snek refute` refutes every blue move
  (762–1286 of them), after blue-win proofs for the red replies tried before it. Seven lines are settled (above).
- **Blue's half, "red cannot win"**: in each of the 12 openings blue keeps one reply, and each of red's answers
  to it (608–1589, 15 180 in all) needs a *step*: a blue move after which every red reply is a unit where blue
  does not lose. A step tries the answers that held earlier in the opening, then the engine's two best, then a
  `REFUTE_LOSS=1` sweep over every blue move.

Pieces drawn at random and run in full, in thread-hours of this laptop at light load. They ran with the solver
before the tight bounds and are divided by the 2.6x the tight bounds gave on 24 of their refutation units and 4
of their steps (2.0–2.3x on the 90 other proof units above):

| piece | samples | median | mean | notes |
|---|---|---|---|---|
| blue's half: step at a random red reply | 48 (4 per opening) | 0.04 | 0.06 | one step open at its cap |
| blue's half: step at one of red's 5 strongest replies | 7 openings | 0.39 | 0.35 | blue's reply loses in 1:5, 1:17 and 1:18 (above); 1:16 open |
| red's half: line at a weak blue reply (151 such replies) | 3 | 0.30 | 0.32 | the hold was red's first reply each time |
| red's half: line at a strong blue reply (18 such replies) | 2 | 0.8 | – | hold at red's 2nd reply; the other line open at its 2nd |

In these lines red's replies were ordered by a 5 s engine search, and the hold came first or second. The three
earlier lines, ordered by the evaluation alone, held only at red's 17th–30th reply and cost 7–20 thread-hours
each with the solver of the time. A prediction written down before one weak line was run (median 2.6, 90 %
0.9–5.1 thread-hours with the solver of the time) overestimated it: the line cost 0.9.

A bootstrap over the samples, stratified by opening and by the engine's rating of blue's reply, with censored
pieces extended by the rest of their own sweep (`results/timeline_model.txt`; a full 16-thread laptop counted as
7–10 light-load threads, an AWS c7a core as 1.0–1.3 per thread-hour at 60–90 % parallel efficiency, c7a in
us-east-1 at $0.9645/h spot and $3.2845/h on demand per 64 cores on 15 Oct 2026):

| scenario | thread-hours, median (90 %) | 16-thread laptop | 128 cores | 1000 cores | AWS spot / on demand |
|---|---|---|---|---|---|
| draw, each opening's preferred blue reply holding | 1 410 (970–2 160) | 7 days | 0.7 days | 2.2 h | $33 / $112 |
| draw, with new blue replies where the preferred one loses | 2 030 (1 250–3 930) | 10 days | 1.0 day | 3.2 h | $48 / $162 |
| draw, also with a centre reply that wins for blue | 3 070 (1 370–5 580) | 15 days | 1.5 days | 4.7 h | $71 / $240 |
| red wins | 1 030–1 220 (260–2 290) | 5–6 days | 0.5–0.6 days | 1.6–1.9 h | $24–28 / $80–96 |

- **The value is open, and a red win is plausible.** Blue's preferred reply loses in three of the seven
  openings tested against red's strongest replies, and in 1:5 the next reply loses too. The engine had rated
  blue 59–83 % in every opening, so its opening evaluation is no guide. The model's chance of a red win (0.44
  or 0.87) comes from a prior, not a measurement.
- **Blue's half dominates a draw proof** (about 80 %); the 3 heaviest of the 48 random steps carry 29 % of it,
  and three pieces are still open, so the tail is uncertain. A run should first test each opening's blue reply
  against red's strongest replies, then replacements, and only then do the bulk of the steps.
- **For comparison**: Hex 9x9 (all openings) took about 6·10⁴ thread-hours in 2012, the 10x10 centre about
  4·10⁴ in 2013, and Pentago's strong solution about 4·10⁵.

These are estimates from samples, not bounds.

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
| `snek arena <pairs> <sec> <threads> <A> <B> [seed]` | engine match; engines `nn`, `nn:<net>`, `ab2`, `ab`, `abhand`, `mcts`, `ab:<weights.txt>`; `ARENA_BOOK=<file>` / `ARENA_BOOK_START=<n>` take the openings from a book, `ARENA_TIME_A=<sec>` gives A other time |
| `python tools/arena_stats.py <logs…>` | pentanomial score, Elo with a 95 % interval and the SPRT log-likelihood ratio from `snek arena` logs |
| `snek book <out> <candidates> <threads> <sec> <seed> [plies] [lo] [hi]` | balanced arena openings (kept when `ab2` scores the side to move within [lo, hi]) |
| `snek datagen <out.csv> <games> <threads> <sec/move> <seed> [engine] [book]` | self-play training data for the network; solves that run out of time are kept as censored |
| `python tools/train_nn.py --snek <exe> --data a.csv b.csv … --out net.bin` | trains the network; `--init net.bin.npz --epochs 0` reports a network on the held-out games, exact-label and blended-target rows apart; `--embed net.bin src/nn_net.hpp` makes it the default |
| `snek firstwin <eval> <data.csv> <ply> <max> <file index>`, `snek evalrank <eval> <move> <moves…>` | rank of the first winning move on held-out positions (`<file index>`: the file's place in the trainer's `--data` list), and of one move, in an evaluation's order (`logistic`, `nn`, `nn:<file>`, `mean:<file>`) |
| `snek gendata <games> <plyMin> <plyMax> <solveSec> <threads> <seed>` | labelled positions (features + exact value) as CSV |
| `python tools/fit_eval.py data/*.csv` | fit the evaluation → `src/eval_weights.hpp`, `data/weights_latest.txt` |
| `snek solvepos <sec> <threads> <moves…>` | parallel exact solve of a position (`PROGRESS=<file>` logs and resumes; `SOLVER_WIN_TEST=1` only proves or refutes that the side to move loses: each root move is `loss` or `notloss`, much cheaper) |
| `snek refute <threads> <log> <moves…>` | shows that the side to move cannot win (`REFUTE_LOSS=1`: that it loses), or finds a move that survives: for every move a reply after which the mover does not win (replies that refuted other moves first, then by the evaluation; `REFUTE_TRIES` × `REFUTE_NODES` M nodes), then a full search over all replies for the moves left open, in chunks of `REFUTE_CHUNK` moves; `REFUTE_FIRST=<file>` settles the listed moves first (answers that won against sibling replies often win again); the moves are taken in the evaluation's order after the 8 best of a `REFUTE_SEARCH` (2) second engine search, which ranks easy winners far higher. Resumable log, headed by the position, `REFUTE_LOSS` and `SOLVER_RED_BONUS` (a log for anything else is refused); every refutation is one position anyone can re-solve |
| `snek proveline <threads> <dir> <moves…>` | settles every reply of the side to move with `refute` for the opponent: an answer that wins against all replies after it, or proof that the opponent cannot win there. Answers that won against earlier replies are tried first; `dir/summary.txt` (one line per reply) and the sweeps' logs make it resumable, so one machine can work through a whole line. It stops at the first reply after which the opponent cannot win, which settles the line (`PROVELINE_ALL=1` goes on), e.g. `snek proveline 16 centre 1:60 4B:16,27,28,39` |
| `snek solvebench <plies> <n> <sec> <threads>` | solver benchmark on random positions (`SOLVER_PNS=1`: values from two `PnSearch` win tests) |
| `snek selfplay <games> <sec> <threads>` | engine vs itself, prints the game |
| `snek verify [games]` | fast move generator == reference checker, incremental snakes and fast bounds == reference, bounds never violated, the one-player optimum between the final snake and the cluster bound, the same per cluster as on the whole board, and never below the real-rules one-player optimum |
| `snek benchset <file> <threads>` | solver benchmark on a fixed position set (`PROFILE=1` prints per-ply statistics, including the tight-bound cutoffs and the one-player questions; `SOLVER_TIGHT_REM` / `SOLVER_TIGHT_ALL_REM` move the tight bounds' gates, 5 and 6 pieces; `BENCH_FRESH=1` starts every position from empty tables for reproducible node counts, `BENCH_WINTEST=1` only asks whether the side to move wins, `BENCH_PNS=1` with it uses `PnSearch`; a build with `-DSOLVER_TIMING` adds exclusive time per ply and per step) |
| `snek perft <d>`, `snek speed`, `snek info` | counting / micro-benchmarks |
| `python ref/crosscheck.py [games]` | engine vs independent Python rules implementation |

### Engine protocol (`snek serve [threads]`, one line in → one JSON line out)

- `new`, `state`, `undo`, `set <m1> <m2> …` (position from the start), `play <m> …` (append moves)
- move notation: `pass` or `<piece>:<cell>,<cell>,…`, cell = `(r+5)*11 + (q+5)` (axial q,r; centre = 60).
  Pieces: `1 2 3I 3V 4I 4A 4B 4C`.
- `analyze <sec>` / `go <sec>` – ranked moves (score = 100·logit of the win probability, ±100000 = proven),
  `go` also plays the best move
- `solve <sec>` – parallel exact solve
- `engine nn|ab2|ab|mcts`

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
- `snek verify 3000` also checks the one-player optimum in 55 259 positions with at most 4 pieces left: never
  below the final snake, never above the cluster bound (strictly below it in 24 272), the same cluster by
  cluster as over the whole board, and never below the real-rules one-player optimum (brute force on 43 191
  positions with at most 3 pieces). An independent SAT and Python brute force agree on 9 382 more player-rows.
- The tight bounds give the solver's values on 1440 random positions after 8–12 plies (red bonus −1/0/+1), 48
  full-window positions after 5–6 plies, the 96 `bench/` units and 90 proof units. An adversarial check added
  21 048 position-bonus pairs at plies 5–13 and red bonuses −3..+3 (full window and both null windows, fresh and
  reused tables), 15 850 against the v3 sources, 4 520 endgames against a rules-only alpha-beta, and a per-node
  check of every tight bound against exact values (36 M nodes): no difference.
- The network's incremental accumulators equal a full refresh on 1.9 M children, its int16 output equals the
  trainer's integer model, and the AVX2 and scalar paths agree.
- `perft`: 1 → 91, 2 → 194 388, 3 → 315 706 848.

## Layout

| Path | What |
|---|---|
| `src/core.hpp` | geometry, pieces, bit-parallel move generation, bounds, notation |
| `src/eval.hpp`, `src/eval_weights.hpp` | features, learned + hand-made evaluation |
| `src/search.hpp` | α-β playing engine |
| `src/nn.hpp`, `src/nn_net.hpp` | network evaluation, compiled-in network |
| `src/datagen.hpp` | self-play data, opening books, held-out rule |
| `src/solver.hpp` | exact solver, parallel solve |
| `src/pns.hpp` | win test that screens the root moves for the cheapest proof |
| `src/mcts.hpp` | v1 MCTS engine (kept for comparison) |
| `src/main.cpp` | CLI + UI protocol |
| `readable/` | the same engine refactored for reading (identical results), with a reading guide: [readable/README.md](readable/README.md) |
| `ref/` | independent Python reference implementation + cross-check |
| `tools/fit_eval.py`, `tools/train_nn.py` | evaluation fitting, network training |
| `tools/arena_stats.py` | match statistics |
| `data/` | solver-labelled positions (`labels_v*.csv`), weight files, `nn/` networks, `book2.txt` arena openings |
| `ui/` | Node server + single-page UI |
| `results/` | benchmark / arena / solve logs |
