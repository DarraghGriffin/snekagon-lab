# Centre run directory

The driver of the centre-opening run writes the run directory; the monitor (`server.py`) only reads it.

| Path | Written by | What |
|---|---|---|
| `<run>/state.json` | driver | the whole run's state (below), rewritten atomically: write `state.json.tmp`, then `os.replace`, at most every 5 s and on every change of a piece's status |
| `<run>/pieces/` | driver | each piece's stdout; `out` fields name these files relative to `<run>` |
| anything else | driver | the driver's own files |

On Windows `os.replace` raises `PermissionError` while another process has `state.json` open (the monitor holds it
for one read; other processes may look at a freshly written file too), and a reader's open can fail while the
replace runs. So the driver retries the replace (as `demo.py` does: up to 20 tries, 50 ms × attempt apart), and the
monitor keeps serving its last good copy.

Moves use the CLI notation `<piece>:<cell>,<cell>,...` (pieces `1 2 3I 3V 4I 4A 4B 4C`; cell `(r+5)*11+(q+5)` for
axial `q, r` in -5..5 with `|q+r| <= 5`, centre 60) or `pass`. Red plays plies 0, 2, 4, …; blue 1, 3, 5, ….
Times are ISO 8601 with offset.

```json
{
  "version": 1,
  "run": {"name": "centre", "root": "1:60", "status": "starting|running|stopping|stopped|done",
          "started": "<ISO>", "updated": "<ISO>", "binary": "<path>", "binarySha256": "<hex>",
          "threads": 14, "busyThreads": 12},
  "summary": {"threadSeconds": 0.0, "piecesTotal": 1761, "piecesDone": 0, "etaSeconds": null,
              "verdict": "open|draw|red-wins|blue-wins", "verdictText": "<one sentence>"},
  "red": {"goal": "<text>", "lines": [
    {"id": "R:<b1>", "b1": "<move>", "rating": 0.42, "status": "pending|running|held|blue-wins|deferred|error",
     "hold": "<r2>|null", "holdRank": 3, "tried": 3, "threadSeconds": 0.0, "source": "run|results/<file>",
     "current": "<r2 being settled>|null", "out": "pieces/<file>|null"}]},
  "blue": {"goal": "<text>", "b1": "<blue reply in use>",
           "b1History": [{"b1": "<move>", "status": "active|failed|replaced", "note": "<text>"}],
           "replies": [
    {"id": "B:<r2>", "r2": "<move>", "rank": 1, "status": "pending|running|held|blue-wins|blue-loses|deferred|error",
     "answer": "<blue move>|null", "threadSeconds": 0.0, "sweep": false, "source": "run|results/<file>",
     "out": "pieces/<file>|null"}]},
  "running": [
    {"id": "R:<b1>|B:<r2>", "half": "red|blue", "moves": "<full move list>", "threads": 4, "started": "<ISO>",
     "phase": "candidates|pass 1|pass 2|ordering|...", "done": 120, "total": 940, "detail": "<text>",
     "out": "pieces/<file>"}],
  "events": [{"t": "<ISO>", "kind": "start|held|blue-wins|blue-loses|deferred|resumed|error|info|verdict",
              "half": "red|blue|run", "id": "<piece id or ''>", "text": "<one line>"}],
  "history": [{"t": "<ISO>", "threadSeconds": 0.0, "piecesDone": 0, "busyThreads": 12}]
}
```

- `red.lines`: the 172 symmetry-distinct blue replies `b1` to `1:60`. A line is `held` once red has a reply `hold`
  after which every blue move is refuted (`holdRank`: its place in red's candidate order, `tried`: red replies
  settled in the line); `blue-wins` means no red reply holds, which breaks red's half. `rating` is the engine's win
  probability for blue (null when unknown).
- `blue.replies`: all of red's replies `r2` after `1:60 <blue.b1>`, `rank` 1 = red's strongest. `held`: blue's
  `answer` leaves red unable to win; `blue-wins`: the answer wins; `blue-loses`: no answer holds, which breaks blue's
  half (`b1History` then records the next `b1`). `sweep`: the answer needed the full sweep.
- `threadSeconds` counts all pieces, imported results included where their cost is known; `source` is `run` or the
  `results/` file a piece was imported from.
- `events` keeps the last 300, oldest first; `history` keeps one point per 5 minutes for the whole run.
