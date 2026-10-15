// Snake lengths and sound bounds on a player's final longest snake.
//
// The bounds are what makes the exact solver fast: once one player's guaranteed length (lower
// bound) beats everything the opponent can still reach (upper bound), the game is decided and
// nothing below that node has to be searched.
#pragma once
#include <functional>
#include "rules.hpp"

inline int longestSnakeIn(Bitboard cells) {
    return largestRegionSize(cells);
}

// The longest snake of `player` if it placed p (a legal placement for that player) now: the piece
// and every own snake it touches become one snake.
inline int longestSnakeAfter(const Position& pos, int player, const Placement& p) {
    int joined = p.cellCount;
    for (int j = 0; j < pos.snakeCount[player]; j++)
        if (pos.snakes[player][j] & p.border) joined += countCells(pos.snakes[player][j]);
    return std::max<int>(pos.longestSnake[player], joined);
}

struct SnakeBounds {
    int lower;  // the longest snake now (snakes never shrink)
    int upper;  // no continuation of the game can give a longer snake
};

// Why the upper bound holds:
//  - A player can never again use a cell next to one of its body cells (that body cell would get
//    a third neighbour). So all future own cells lie in growable = empty minus such cells.
//  - The final longest snake is connected, so it lies inside one connected cluster of
//    (own cells + growable cells).
//  - New cells come in whole pieces and every piece lies inside one connected region of
//    growable cells, so a cluster gains at most bestFill(sum over its regions of bestFill(region)).
//  - Every old snake ends up in the final snake whole or not at all, and along the final snake
//    old snakes and new pieces alternate. With k pieces left it therefore contains at most k + 1
//    old snakes: only the k + 1 longest snakes of a cluster count.
//
// Implementation: clusters are built from the growable regions, merged by the snakes touching
// them (regions are small blobs and cheap to flood; snakes are only attached by adjacency).

// A player's clusters, which the solver keeps to look for threats (solver.hpp).
struct SnakeClusters {
    int count = 0;
    Bitboard cells[BOARD_CELLS];    // the growable cells and own snakes of each cluster
    int upper[BOARD_CELLS];         // the longest final snake possible inside each cluster
    int regionCount = 0;
    Bitboard regions[BOARD_CELLS];  // the connected regions of growable cells (of all clusters)
};

// The cells `player` may still occupy: empty and not next to its snake body.
inline Bitboard growableCells(const Position& pos, int player) {
    return pos.emptyCells() & ~neighboursOf(snakeShapeOf(pos.cells[player]).body);
}

// Upper bound (at least `lower`) on the final longest snake of a player with the pieces `left`,
// the snakes snakes[0 .. snakeCount) and the growable cells `growable`, cluster by cluster as
// described above. With `record`, the clusters are kept there as well.
inline int clusterUpperBound(Bitboard growable, const Bitboard* snakes, int snakeCount, PieceSet left, int lower,
                             SnakeClusters* record) {
    if (record) record->count = record->regionCount = 0;
    Bitboard clusterCells[BOARD_CELLS];
    int ownCells[BOARD_CELLS], newCells[BOARD_CELLS];  // own cells / most new cells per cluster
    int clusters = 0;
    forEachRegion(growable, [&](Bitboard region) {
        clusterCells[clusters] = region;
        ownCells[clusters] = 0;
        newCells[clusters] = pieceTables.bestFill[left][countCells(region)];
        clusters++;
        if (record) record->regions[record->regionCount++] = region;
    });

    for (int j = 0; j < snakeCount; j++) {
        Bitboard snake = snakes[j], around = neighboursOf(snake);
        int into = -1;  // the cluster this snake joins
        for (int i = 0; i < clusters; i++) {
            if (i == into || !(clusterCells[i] & around)) continue;
            if (into < 0) { into = i; continue; }
            // the snake connects cluster i with cluster `into`: merge them, then move the last
            // cluster into the freed slot i and look at slot i again
            clusterCells[into] |= clusterCells[i];
            ownCells[into] += ownCells[i];
            newCells[into] += newCells[i];
            clusters--;
            clusterCells[i] = clusterCells[clusters];
            ownCells[i] = ownCells[clusters];
            newCells[i] = newCells[clusters];
            if (into == clusters) into = i;
            i--;
        }
        if (into >= 0) {
            clusterCells[into] |= snake;
            ownCells[into] += countCells(snake);
        }
    }

    // more snakes than one final snake can join: only the k + 1 longest of a cluster count
    int joinable = pieceCount(left) + 1;
    if (snakeCount > joinable) {
        for (int i = 0; i < clusters; i++) {
            int lengths[MAX_SNAKES], inCluster = 0;
            for (int j = 0; j < snakeCount; j++)
                if (clusterCells[i] & snakes[j]) lengths[inCluster++] = countCells(snakes[j]);
            if (inCluster <= joinable) continue;
            std::partial_sort(lengths, lengths + joinable, lengths + inCluster, std::greater<int>());
            ownCells[i] = 0;
            for (int j = 0; j < joinable; j++) ownCells[i] += lengths[j];
        }
    }

    int upper = lower;
    for (int i = 0; i < clusters; i++) {
        int reachable = ownCells[i] + pieceTables.bestFill[left][std::min(newCells[i], GRID_SIZE)];
        upper = std::max(upper, reachable);
        if (record) {
            record->cells[record->count] = clusterCells[i];
            record->upper[record->count++] = reachable;
        }
    }
    return upper;
}

// The bounds of `player`; with `clusters`, its clusters are kept there as well.
inline SnakeBounds snakeBounds(const Position& pos, int player, SnakeClusters* clusters = nullptr) {
    SnakeBounds bounds;
    bounds.lower = pos.longestSnake[player];
    if (clusters) clusters->count = 0;
    PieceSet left = pos.piecesLeft[player];
    if (!left) { bounds.upper = bounds.lower; return bounds; }
    bounds.upper = clusterUpperBound(growableCells(pos, player), pos.snakes[player], pos.snakeCount[player], left,
                                     bounds.lower, clusters);
    return bounds;
}

// The upper bound of `player` after the opponent occupies the empty cells `blocked` (one
// placement), given the player's clusters before that. Only the clusters that `blocked` touches
// can change, and only those are recomputed - unless the block provably changes nothing: a piece
// lies inside one region of growable cells, and when that region stays connected without it (the
// cells next to the piece are still connected within distance 2 of it), still holds as many piece
// cells, and still touches every snake it touched, the cluster's bound is the same.
inline int upperBoundAfterBlock(const Position& pos, int player, const SnakeClusters& clusters, int lower, Bitboard blocked) {
    int upper = lower;
    PieceSet left = pos.piecesLeft[player];
    Bitboard own = pos.cells[player];
    for (int i = 0; i < clusters.count; i++) {
        Bitboard cluster = clusters.cells[i];
        if (!(cluster & blocked)) {
            upper = std::max(upper, clusters.upper[i]);
            continue;
        }
        Bitboard snakesIn[MAX_SNAKES];
        int snakesInCount = 0;
        for (int j = 0; j < pos.snakeCount[player]; j++)
            if (cluster & pos.snakes[player][j]) snakesIn[snakesInCount++] = pos.snakes[player][j];

        Bitboard blockedGrowable = blocked & cluster & ~own;
        Bitboard region = 0;  // the growable region the block lies in
        for (int k = 0; k < clusters.regionCount; k++)
            if (clusters.regions[k] & blockedGrowable) { region = clusters.regions[k]; break; }
        Bitboard regionAfter = region & ~blocked, besideBlock = neighboursOf(blocked) & regionAfter;
        bool unchanged = besideBlock && !(blockedGrowable & ~region) &&
                         pieceTables.bestFill[left][countCells(regionAfter)] == pieceTables.bestFill[left][countCells(region)];
        if (unchanged) {
            Bitboard nearBlock = neighboursOf(neighboursOf(blocked)) & regionAfter;
            Bitboard connected = floodFill(cellBit(firstCell(besideBlock)), nearBlock);
            unchanged = !(besideBlock & ~connected);
            for (int j = 0; j < snakesInCount && unchanged; j++) {
                Bitboard around = neighboursOf(snakesIn[j]);
                if ((around & region) && !(around & regionAfter)) unchanged = false;
            }
        }
        Bitboard growableAfter = cluster & ~own & ~blocked;
        upper = std::max(upper, unchanged ? clusters.upper[i] : clusterUpperBound(growableAfter, snakesIn, snakesInCount, left, lower, nullptr));
    }
    return upper;
}

// The same bounds computed the slow, obvious way (flooding own + growable cells directly).
// `snek verify` checks that snakeBounds() always agrees with it.
inline SnakeBounds snakeBoundsReference(const Position& pos, int player) {
    Bitboard own = pos.cells[player];
    SnakeBounds bounds;
    bounds.lower = longestSnakeIn(own);
    PieceSet left = pos.piecesLeft[player];
    if (!left) { bounds.upper = bounds.lower; return bounds; }
    SnakeShape shape = snakeShapeOf(own);
    Bitboard growable = pos.emptyCells() & ~neighboursOf(shape.body);
    bounds.upper = bounds.lower;
    forEachRegion(own | growable, [&](Bitboard cluster) {
        Bitboard growableInCluster = cluster & growable;
        if (!growableInCluster) return;
        int capacity = 0;
        forEachRegion(growableInCluster, [&](Bitboard region) { capacity += pieceTables.bestFill[left][countCells(region)]; });
        std::vector<int> snakeLengths;
        forEachRegion(cluster & own, [&](Bitboard snake) { snakeLengths.push_back(countCells(snake)); });
        std::sort(snakeLengths.begin(), snakeLengths.end(), std::greater<int>());
        int joinable = std::min<int>(snakeLengths.size(), pieceCount(left) + 1);
        int ownReachable = 0;
        for (int j = 0; j < joinable; j++) ownReachable += snakeLengths[j];
        int reachable = ownReachable + pieceTables.bestFill[left][std::min(capacity, GRID_SIZE)];
        bounds.upper = std::max(bounds.upper, reachable);
    });
    return bounds;
}
