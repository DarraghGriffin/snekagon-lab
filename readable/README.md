# Readable version of the engine

This folder holds the same program as `src/`, rewritten to be read by people: smaller files by topic,
descriptive names, short functions, and comments that explain *why*. Its behaviour and results are the
same as those of `src/`. Every deterministic output was compared and is identical (see
[Verification](#verification)).

`src/` stays the production version: `build.sh`, the UI (`ui/server.js`) and the distributed workers use
it. Use this folder to understand the code, or as a drop-in replacement; the command line is the same.

```bash
g++ -O3 -march=native -std=c++20 -pthread -static -o build/snek_readable.exe readable/*.cpp -lws2_32   # Windows (MinGW)
g++ -O3 -march=native -std=c++17 -pthread -o build/snek_readable readable/*.cpp                         # Linux
```

The learned evaluation weights are not duplicated: `evaluation.hpp` includes `../src/eval_weights.hpp`,
which `tools/fit_eval.py` writes.

## Reading order

| # | File | What it contains |
|---|------|------------------|
| 1 | `board.hpp` | Hex coordinates, the 128-bit `Bitboard`, neighbours by shifting, flood fill, the 12 symmetries |
| 2 | `pieces.hpp` | The 8 pieces, their orientations, all placements (numbered once at start-up), `Move` |
| 3 | `position.hpp` | `Position`, `play()`, snakes kept up to date incrementally, Zobrist hashing, symmetric keys |
| 4 | `rules.hpp` | The placement rules: a plain rule check (for either player) and the fast bit-parallel move generator |
| 5 | `bounds.hpp` | Sound lower and upper bounds on each player's final longest snake, the clusters behind them, and the exact one-player optimum (the longest snake a player could still make if the opponent never moved again) |
| 6 | `features.hpp` | Territory split ("who gets where first") and per-player features |
| 7 | `evaluation.hpp` | Learned evaluation (logistic regression over 14 features) and the old hand-made one |
| 8 | `transposition_table.hpp` | Lockless two-slot hash table of the exact solver |
| 9 | `solver.hpp` | **Exact solver**: alpha-beta over {-1, 0, +1} with a window, threat zones and the last pieces valued without a search, the one-player optimum and guaranteed growth near the end, split into steps (table, last pieces, bounds, tight bounds, threats, ordering, search, store) |
| 10 | `parallel_solve.hpp` | Exact solve of a position, or a yes/no question about it, with its root moves spread over threads |
| 11 | `screened_win_test.hpp` | Win test that looks for cheap proofs first: the best moves are screened by small probes of their replies |
| 12 | `engine.hpp` | **Playing engine**: iterative-deepening PVS with forward pruning, multi-PV, score smoothing; in `ab2` also exact verdicts on the root moves |
| 13 | `analysis.hpp` | "Analyse this position" for the UI: exact when few pieces are left, otherwise the engine |
| 14 | `refute.hpp` | **Refutation sweep**: does the side to move win, or can each of its moves be refuted? Node-limited tries of likely refutations, then full yes/no solves, on chunks of moves; resumable log |
| 15 | `mcts.hpp` | The first engine (Monte-Carlo tree search), kept for comparison |
| 16 | `notation.hpp`, `training_data.hpp` | Moves and positions as text; CSV rows for training the evaluation |
| 17 | `main.cpp`, `commands.hpp` | Table of all commands with their arguments |
| 18 | `cmd_*.cpp` | One file per group of commands: `serve` (UI protocol), `solve` (solvepos, solvebench, benchset), `refute` (refute, proveline), `check` (perft, verify, info, speed), `data` (gendata, refeature, evalexplain, evaltrace), `play` (arena, selfplay), `worker` (distributed solving) |

To see how a position gets solved, read `rules.hpp`, then `bounds.hpp`, then `solver.hpp`, and
`screened_win_test.hpp` for the win test that picks cheap proofs. To see where the engine's move comes
from, read `evaluation.hpp`, then `engine.hpp`, then `analysis.hpp`. To see how a line of play gets
proved one reply at a time, read `parallel_solve.hpp`, then `refute.hpp`, then `proveline` in
`cmd_refute.cpp`.

## Names: original to readable

| `src/` | `readable/` |
|--------|-------------|
| `BB` | `Bitboard` |
| `Pos`, `s` | `Position`, `pos` |
| `own[c]`, `avail[c]`, `stm`, `passes` | `cells[player]`, `piecesLeft[player]`, `sideToMove`, `passesInRow` |
| `comp`, `ncomp`, `L` | `snakes`, `snakeCount`, `longestSnake` |
| `h[t]`, `key(t)` | `hash[t]`, `canonicalKey(symmetry)` |
| `T` (one table struct) | `geometry`, `pieceTables`, `zobrist` |
| `T.PL[p]`, `gid` | `placementsOf(p)`, `placementId` (all placements in one vector) |
| `T.gidSym`, `T.perm`, `T.inv` | `symmetricPlacement`, `geometry.symmetric`, `geometry.inverseSymmetry` |
| `T.fit` | `pieceTables.bestFill` |
| `expand`, `shiftFrom`, `flood`, `popc`, `lsb` | `neighboursOf`, `cellsWithOffsetIn`, `floodFill`, `countCells`, `firstCell` |
| `Shape` (`ge1`, `ge2`, `ge3`, `tri2`, `full`) | `SnakeShape` (`touches1..3`, `touchesAdjacentPair`, `body`) |
| `GenCtx`, `orientLegal` | `MoveGenContext`, `legalAnchors` |
| `Orient` (`ideg`, `tri`, `two`, `gidAt`) | `Orientation` (`innerNeighbours`, `wedgeCells`, `bridgeCells`, `placementAt`) |
| `s.genMoves`, `s.hasMove`, `s.legal`, `s.legalMove` | `generateMoves`, `hasLegalMove`, `isLegalPlacement`, `isLegalMove` |
| `s.legalFor(c, pl)`, `s.forEachLegalTouching(cells, f)` | `isLegalPlacementFor(pos, player, p)`, `forEachLegalMoveCovering(pos, cells, f)` |
| `snakeBounds(s, c, lb, ub)` | `SnakeBounds snakeBounds(pos, player)` with `.lower`, `.upper` |
| `Clusters` (`n`, `ub`, `region`), `snakeClusters`, `clusterBound` | `SnakeClusters` (`count`, `upper`, `regions`), `snakeBounds(pos, player, &clusters)`, `clusterUpperBound` |
| `growable`, `boundAfterBlock`, `lengthAfter` | `growableCells`, `upperBoundAfterBlock`, `longestSnakeAfter` |
| `voronoi`, `PlayerFeat`, `features` | `splitTerritory`, `PlayerFeatures`, `computeFeatures` |
| `EW`, `EHEAD`, `g_evalSet`, `g_handEval`, `evaluateHand` | `featureWeights`, `featureHeadStart`, `activeWeightSet`, `useHandMadeEval`, `evaluateHandMade` |
| `LTT` | `TranspositionTable` |
| `Solver::terminalValue`, `finalScore` | `gameResult`, `redLead` |
| `Threat`, `addThreat`, `scanThreats`, `threatBound` | `Threat`, `ThreatList::tryPlacement`, `ThreatList::scan`, `ThreatList::boundFor` |
| `OnePlayer` (`mayReach`, `reachIn`, `dfs`, `queries`, `unreachableAnswers`, `dfsNodes`), `DFS_BUDGET` | `OnePlayerOptimum` (`mayReach`, `reachableInCluster`, `extend`, `stats.questions`, `.unreachable`, `.searchNodes`), `SEARCH_BUDGET` |
| `Orient::nextTo`, `orientGrowLegal`, `forEachLegalGrow` | `Orientation::anchorsNextTo`, `growingAnchors`, `forEachGrowingMove` |
| `Solver::growthBounds`, `tightenBounds`, `tightRem`, `pTight` | `guaranteedGrowth`, `Solver::narrowByTightBounds`, `tightBoundsUpToPieces`, `profile.tightCut` |
| `lastPieces` (`-2`: not the last pieces) | `lastPiecesValue` (`false`) |
| `pPruned`, `pMoves`, `pLast`, `pScans`, `pScanHits` | `profile.threatSkipped`, `.movesListed`, `.lastPieces`, `.threatScans`, `.threatScansFound` |
| `PART_*`, `PlyTimer`, `TIMED` (`-DSOLVER_TIMING`) | `Solver::Step`, `NodeTimer`, `timed()` |
| `ab.search` + `nodeLimit` / `useDeadline` (as `PnSearch` and `solveWindow` use them) | `Solver::searchWithin`, `setTimeLimit`, `setDeadline`, `pastDeadline` |
| `ParallelSolveResult`, `SolveQuestion` | `RootSolveResult`, `SolveQuestion` (an `enum class`) |
| `PnSearch` (`pns.hpp`), `ab`, `solveWin`, `solveValue`, `totalNodes` | `ScreenedWinTest` (`screened_win_test.hpp`), `solver`, `attackerWins`, `value`, `valueNodes` |
| `solveOr`, `solveAnd`, `moves`, `full`, `sampleOrder`, `scaled`, `pnNodes` | `someMoveWins`, `everyReplyLoses`, `unrefutedMoves`, `fullTest`, `spreadSampleOrder`, `budgetFor`, `orderingNodes` |
| `AbSearcher::ab`, `exactKnown`, `P` | `AlphaBetaSearcher::search`, `exactScore`, `params` |
| `AbParams`, `abParamsV2()` | `EngineParams`, `engineParamsV2()` |
| `AbEngine` (`pool`, `exactTT`, `exactPool`), `abEngine(version)` | `EngineVersion` (`searchers`, `verdictTable`, `verdictSolvers`), `engineVersion(version)` |
| `abAnalyze`, `RootResult`, `AbAnalysis`, `rootMovesByEval` | `analyzeWithEngine`, `RootMoveResult`, `EngineAnalysis`, `scoreRootMovesAtDepth1` |
| `solveWindow`, `exactVerdicts`, `rankByVerdict` | `solveWithin`, `exactVerdicts`, `rankByVerdict` |
| `analyzeAb`, `g_engine` | `analyzeAlphaBeta`, `DEFAULT_ENGINE` / `ServedGame::engine` |
| `terminalScore` | `finishedGameScore` |
| `uniqueRootMoves` | `symmetryUniqueMoves` |
| `refuteMoves`, `RefuteResult` (`refute.hpp`) | `refuteMoves`, `RefuteResult`; the sweep is a `RefutationSweep` |
| `quickPass`, `fullPass`, `killers`, `SEARCH_TOP`, `searchSecs` | `quickPass` (`tryRefutations` per move), `fullPass`, `refutations`, `SEARCH_FAVOURITES`, `searchSeconds` |
| proveline's `done`, `answers` | `settled`, `winningAnswers` |
| `moveStr`, `posStr`, `parsePosStr` | `moveToString`, `positionToString`, `parsePosition` |
| `-9` | `Solver::UNKNOWN` |

The feature names of the learned evaluation (`dL`, `dSingle`, ...) and the CSV column names stay as they
are, because weight files and `tools/fit_eval.py` use them. So do the environment variables
(`SOLVER_*`, `BENCH_*`, `PNS_*`, `REFUTE_*`) and the step names of the timing profile.

## What must not change (or results change)

Some details look arbitrary but decide the exact node counts, the move order and so the results:

- **Placement numbering**: piece by piece, orientations in the order the symmetries produce them,
  anchor cells ascending. The move generator returns moves in this order.
- **Zobrist keys**: seed `0x5eca9011`, generated in the order red cells, red pieces, blue cells, blue
  pieces, side to move. Symmetric hashes are kept only up to ply 6 (`SYMMETRY_PLY`).
- **Move ordering**: all scores (killers, `+256` / `+128` / `+32` bonuses, evaluation × 1000; the
  engine's history), the selection sort of the solver's first three moves that keeps the earlier move
  on ties, the one sort of the rest by score and then placement id, and `std::sort` (not stable) on
  the same input arrays (also the threats by zone size, the last pieces' replies and the replies the
  refutation sweep tries).
- **Threat lists**: at most 6 threats per node, 8 placements tried one by one, 64 found per scan; the
  killers of the next ply are tried before the moves are generated.
- **Transposition-table layout** and its replacement rule.
- **Floating point**: the logit is summed in feature order, and `learnedLogit` and the sum that
  `evalexplain` prints stay out of line, so that every caller rounds the same, and as in `src/`
  (inlined, the compiler fuses the multiply-adds in some callers and not in others);
  `(int)(1000 * logit)` truncates and `lround(100 * z)` rounds, as before.
- **Output formats**: JSON for the UI, CSV for training data, the worker protocol, the `solvepos`
  progress lines, the `refute` log, the `proveline` summary and its log names.

## Differences from `src/`

- An unknown command prints the list of commands (`src/` silently exits).
- Missing required arguments of `solvepos` and `evaltrace` fall back to defaults instead of crashing.

## Verification

Both builds (`build/snek_orig.exe` from `src/`, `build/snek_readable.exe` from here, the commands
above) were run on the same inputs and the outputs compared with timings removed. All were identical:

| Check | What it covers |
|-------|----------------|
| `perft 3` | 91 / 194 388 / 315 706 848: move generation |
| `verify 2000`, `verify 3000` | 31 391 / 47 131 positions: fast generator vs rule check, fast vs reference bounds, bounds vs final results |
| `info`, `speed` | placement counts; the checksums of the hot functions |
| `solvebench` 8 6, 10 10, 7 12, 6 12 and 5 3 (1 thread) | values and node counts of the exact solver; also with `SOLVER_PNS=1`, `SOLVER_RED_BONUS=±1`, `SOLVER_THREATS=0`, `SOLVER_THREAT_SCAN=0 SOLVER_LAST=0 SOLVER_KILLERS_OVER_EVAL=0` and `SOLVER_ETC_REM=8` |
| `BENCH_FRESH=1 PROFILE=1 benchset` on the four `bench/ply5_*` sets (1 thread) | exact values, `BENCH_WINTEST=1` and `BENCH_WINTEST=1 BENCH_PNS=1`: node counts per position, per-ply statistics, probes and screens. On `ply5_blue_wins.txt` 28 568 903, 25 417 308 and 12 768 835 nodes |
| the same with `-DSOLVER_TIMING` | node counts and statistics; the timing table has the same rows and steps |
| `solvepos` at plies 6 and 7 (1 thread), also with `SOLVER_WIN_TEST=1`, a `PROGRESS` log and its resume | root-move results and node counts |
| `refute 1 <log>` after `1:60 4B:16,27,28,39 3V:40,41,50 4A:62,72,82,93 4A:58,59,68,78 3V:31,42,52 4I:7,8,9,10` (blue cannot win) and after `1:60 4B:16,27,28,39 3V:29,40,50 4A:6,7,8,19 2:89,99 4C:49,59,70,71 3I:94,104,114` (blue wins); also with `REFUTE_FIRST`, `REFUTE_SEARCH=0`, `REFUTE_LOSS=1` and small `REFUTE_CHUNK` / `REFUTE_TRIES` / `REFUTE_NODES` (both passes), and resumed from part of a log | output and log: every refutation, open move and survivor |
| `proveline 1 <dir>` after `1:60 4B:16,27,28,39 3V:29,40,50 4A:6,7,8,19 2:89,99 4C:49,59,70,71 3I:94,104,114 2:20,21`, resumed after 100 of its 223 replies, and its refusal of `REFUTE_LOSS=1` | output, summary and the sweeps' logs |
| `serve 1`: state, set, play, undo, children, engine, analyze and go (`ab` and `ab2`, where their answer does not depend on time), solve, errors, `SERVE_PROGRESS=1` | UI protocol |
| `gendata` (normal and `good` mode, 1 thread), `refeature`, `evalexplain`, `evaltrace 3` | training rows; features, learned evaluation, fixed-depth search of `ab` |
| fixed-depth `analyzeWithEngine` / `abAnalyze` of `ab` and `ab2` on two games (1 thread, multi-PV 1 and 8, depths 1-4), from a small test program calling them | depth, scores, displayed scores, verdicts and nodes of every root move |
| `worker` against a local test coordinator, also with `SOLVER_WIN_TEST=1` | protocol, values and nodes, node budgets, invalid units |

The one-player optimum and the guaranteed growth bounds were ported later. For them, `verify 3000`,
`BENCH_FRESH=1 BENCH_WINTEST=1 benchset` on the four `bench/ply5_*` sets (also with `BENCH_PNS=1` on
`ply5_blue_wins.txt` and `ply5_other_openings.txt`) and `solvebench` 9 96 (red bonus -1, 0, +1) and
6 12 (1 thread) were compared again: identical. With them, `BENCH_WINTEST=1` on `ply5_blue_wins.txt`
takes 3 811 277 nodes, and `BENCH_PNS=1` 3 105 446.

With several threads the order of the work depends on timing, so those runs were compared by their
results: `solvepos` with 4 threads (exact, and `SOLVER_WIN_TEST=1` at ply 5), `refute 2` (the same
verdict for every move) and `benchset` with 4 threads (the same values). The engine search that picks
`refute`'s first moves stops after `REFUTE_SEARCH` seconds, so its favourites could depend on timing;
in the positions above they were the same in every run. Timing-dependent commands (`arena`,
`selfplay`, MCTS) cannot be compared output to output. They run, and their results are the same within
normal variation. Single-thread `benchset` times of the two builds, run in turns, are within noise of
each other.
