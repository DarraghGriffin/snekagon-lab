#!/usr/bin/env python3
"""Proves the value of the centre opening (red's 1-hex on cell 60) on one machine.

A resumable driver for `snek proveline` and `snek refute`. A draw needs both halves:

- red's half, "blue cannot win": for each of the 172 symmetry-distinct blue replies b1 to 1:60, a
  line: a red reply after which every blue move is refuted, after blue-win proofs for the red replies
  tried before it (PROVELINE_SEARCH=5 snek proveline, which stops at the first reply that holds).
  Lines are taken hard first (the engine's rating of b1 for blue); a line that runs past its
  first-pass cap is deferred and resumed without a cap once no line is pending.
- blue's half, "red cannot win": one blue reply b1 (4B:16,27,28,39; 3V:36,37,48 and then
  4A:51,52,63,74 if a step loses) and for each of red's replies r2 a step: a blue answer after which
  red cannot win (REFUTE_LOSS=1 snek refute, the answers that held most often first). Red's strongest
  replies by a 5 s search come first and run without a cap; the rest have a first-pass cap.

A red line the attacker wins (blue beats the centre) ends the run with "blue-wins"; a blue step that
loses moves blue's half to the next blue reply. Results settled before this driver are imported, each
checked against its log and recorded with its source path.

  python tools/centre_solve.py --snek build/snek.exe --run build/centre_run --threads 14   (a snek with uniquemoves)
  python tools/centre_solve.py --run build/centre_run --stop

Rerunning the first command resumes: finished pieces come from pieces.jsonl, running ones from their
resumable logs. --stop (or a STOP file in the run directory, or Ctrl+C) stops the solver processes and
the driver. The run directory holds:

  state.json     progress for monitors (format: README, "Centre solve driver"), rewritten atomically
  pieces.jsonl   append-only: a record per finished, deferred, stopped, failed or imported piece
  events.jsonl, history.jsonl   append-only events and 5-minute progress points
  setup.json     the piece lists and their order, built on the first start
  pieces/        the solver processes' logs and stdout: R_<b1>/ (proveline), B_<b1>/<r2>.log (refute)
  RESULT.txt     the verdict once it is known

Solver processes run at below-normal priority and die with the driver; while the driver runs, Windows
is asked not to sleep (SetThreadExecutionState, no system setting is changed).
"""
import argparse
import ctypes
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import traceback
from pathlib import Path

CENTRE = "1:60"
BLUE_CANDIDATES = ["4B:16,27,28,39", "3V:36,37,48", "4A:51,52,63,74"]  # blue's half, in this order
STATE_SECONDS = 5.0      # state.json is rewritten at most this often (and on every status change)
HISTORY_SECONDS = 300.0  # one history point per 5 minutes
EVENTS_KEPT = 300
MAX_ERRORS = 2           # a piece whose process fails this often is left as an error
MAX_LOOP_FAILURES = 30   # the driver stops after this many failed passes of its loop in a row
PRIOR_THREAD_SECONDS = {"red": 1800.0, "blue": 300.0}  # ETA per piece before any piece of a half finished here

REPO = Path(__file__).resolve().parent.parent

# Settled before this driver (paths relative to the repository), checked when imported.
# Red's half: b1, the line's summary (snek proveline or the timeline probe; None: the hold was red's
# first reply), the red reply that holds, and the refute log that refutes every blue move after it.
RED_IMPORTS = [
    ("4B:16,27,28,39", "results/centre_4B_proveline.txt", "4C:40,41,50,52", "results/hold_4C40_refute.log"),
    ("4A:51,52,63,74", "results/centre_4A51_proveline.txt", "3V:29,40,50", "results/hold_4A51_3V29_refute.log"),
    ("3V:36,37,48", "results/centre_3V36_proveline.txt", "4A:58,59,68,78", "results/hold_3V36_4A58_refute.log"),
    ("4C:26,36,47,48", "results/centre_4C26_line.txt", "4A:58,59,68,78", "results/hold_4C26_4A58_refute.log"),
    ("4A:18,19,30,41", None, "4A:42,50,51,52", "results/hold_4A18_4A42_refute.log"),
    ("3V:55,56,67", None, "4C:71,73,82,83", "results/hold_3V55_4C71_refute.log"),
    ("3V:35,36,47", None, "4A:18,28,38,49", "results/hold_3V35_4A18_refute.log"),
]
# Blue's half, per b1: the proveline summary whose "wins" lines are blue wins after those red replies,
# and red replies where blue holds the draw (REFUTE_LOSS=1 refute logs ending "<answer> holds").
BLUE_IMPORTS = {
    "4B:16,27,28,39": ("results/centre_4B_proveline.txt", [("4C:40,41,50,52", "results/hold_4C40_draw_refute.log")]),
    "3V:36,37,48": ("results/centre_3V36_proveline.txt", [("4A:58,59,68,78", "results/hold_3V36_4A58_draw_refute.log")]),
    "4A:51,52,63,74": ("results/centre_4A51_proveline.txt", [("3V:29,40,50", "results/hold_4A51_3V29_draw_refute.log")]),
}
# Pilot steps of blue's half after 4B:16,27,28,39 (build/ is not tracked; missing files are skipped):
# tl_p2 bstep logs, "<r2>.log" or "1_60_<r2>.log" with ':' and ',' as '_'. bstep proves an answer with
# a win test of red after it ("holds": red does not win) or with the REFUTE_LOSS=1 sweep; either way
# the log's header names the position and REFUTE_LOSS=1 and its last line for the answer is "holds".
PILOT_STEP_DIRS = [("build/centre_q/steps", ""), ("build/timeline/p2/bsteps", "1_60_")]  # (directory, file prefix)
PILOT_COSTS = "build/centre_q/analysis.txt"  # thread-seconds of those steps, resumes included
# tl_probe "hold" steps: "proves" is a win test of red after blue's answer that red does not win.
PILOT_HOLD = "build/timeline/pilot_hold.txt"
B1_RATINGS = "build/timeline/b1strength_centre.txt"  # "<b1> blue <win probability> ..." per blue reply

BELOW_NORMAL_PRIORITY_CLASS = 0x00004000
CREATE_NO_WINDOW = 0x08000000
ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001

RE_SURVIVES = re.compile(r"^the side to move (does not lose|WINS) with (\S+)$")
RE_ALL_REFUTED = re.compile(r"^the side to move (LOSES|cannot win): all (\d+) moves refuted$")
RE_PARTIAL = re.compile(r"^(\d+) of (\d+) moves refuted, rest unknown$")
RE_WRONG_LOG = re.compile(r'^\S+ starts with ".*", not ".*"$')
RE_ATTACKER = re.compile(r"^(\d+) attacker moves, (\d+) settled earlier$")
RE_PASS1 = re.compile(r"^(\d+)/(\d+) (\S+) (refuted|open)$")
RE_PASS2 = re.compile(r"^pass 2: (\S+) (\S+)$")
RE_REPLY = re.compile(r"^reply (\d+)/(\d+) (\S+)$")
RE_LINE_HELD = "the attacker does not win this line"
RE_LINE_WON = "the attacker WINS this line"
ATTEMPT_MARK = "== attempt "


def now_iso():
    return dt.datetime.now().astimezone().isoformat(timespec="seconds")


def safe_name(move):
    return move.replace(":", "_").replace(",", "_")


def move_from_safe(name):
    piece, _, cells = name.partition("_")
    return piece + ":" + cells.replace("_", ",")


def write_atomic(path, text):
    """Writes a temporary file and renames it over `path`. On Windows the rename fails while another
    process holds `path` open without delete sharing (a reader), so it is retried for a while."""
    tmp = Path(str(path) + ".tmp")
    tmp.write_text(text, encoding="utf-8")
    for attempt in range(40):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if attempt == 39:
                raise
            time.sleep(0.05)


def append_jsonl(path, record):
    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(record, separators=(",", ":")) + "\n")


def read_jsonl(path):
    out = []
    if Path(path).exists():
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            try:
                out.append(json.loads(line))
            except ValueError:
                pass  # a line cut short by a crash
    return out


def trim_partial_line(path):
    """Cuts a last line that a killed process left without its newline (logs are appended to)."""
    try:
        with open(path, "rb+") as f:
            data = f.read()
            if data and not data.endswith(b"\n"):
                f.truncate(data.rfind(b"\n") + 1)
    except FileNotFoundError:
        pass


def last_attempt(text):
    """The part of a stdout file written by its last attempt."""
    at = text.rfind(ATTEMPT_MARK)
    return text[at:] if at >= 0 else text


# ---- solver output ---------------------------------------------------------------------------------


def parse_refute_output(text):
    """The verdict of a `snek refute` run from its stdout (the last attempt):
    {"kind": "survives"|"refuted-all"|"partial"|"error"|"none", "loss": bool, "answer", "total", "refuted", "line"}.
    survives: "does not lose" (REFUTE_LOSS=1) or "WINS"; refuted-all: "LOSES" or "cannot win"."""
    result = {"kind": "none", "loss": None, "answer": None, "total": None, "refuted": None, "line": ""}
    for line in last_attempt(text).splitlines():
        line = line.strip()
        if m := RE_SURVIVES.match(line):
            result.update(kind="survives", loss=m.group(1) == "does not lose", answer=m.group(2), line=line)
        elif m := RE_ALL_REFUTED.match(line):
            result.update(kind="refuted-all", loss=m.group(1) == "LOSES", total=int(m.group(2)), refuted=int(m.group(2)), line=line)
        elif m := RE_PARTIAL.match(line):
            result.update(kind="partial", refuted=int(m.group(1)), total=int(m.group(2)), line=line)
        elif RE_WRONG_LOG.match(line) or line.startswith("illegal move"):
            result.update(kind="error", line=line)
    return result


def parse_proveline_summary(path):
    """A proveline summary (or a timeline-probe line summary): header, [(reply, "wins"|"holds", answer)]."""
    header, entries = "", []
    p = Path(path)
    if not p.exists():
        return header, entries
    for line in p.read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            header = header or line
            continue
        f = line.split()
        if len(f) >= 2 and f[1] in ("wins", "holds", "unknown"):
            entries.append((f[0], f[1], f[2] if f[1] == "wins" and len(f) > 2 else None))
    return header, entries


def parse_proveline_output(text):
    """The verdict of a `snek proveline` run from its stdout (the last attempt):
    {"kind": "held"|"attacker-wins"|"partial"|"error"|"none", "line"}."""
    result = {"kind": "none", "line": ""}
    for line in last_attempt(text).splitlines():
        line = line.strip()
        if line == RE_LINE_HELD:
            result.update(kind="held", line=line)
        elif line == RE_LINE_WON:
            result.update(kind="attacker-wins", line=line)
        elif re.match(r"^\d+ replies: \d+ with a winning answer", line) and result["kind"] == "none":
            result.update(kind="partial", line=line)
        elif RE_WRONG_LOG.match(line) or line.startswith("illegal move") or line.startswith("proveline proves wins"):
            result.update(kind="error", line=line)
    return result


def refute_log_status(path):
    """header (or ""), {move: last status} of a refute log."""
    header, status = "", {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if line.startswith("#"):
            header = header or line
            continue
        f = line.split()
        if len(f) >= 2:
            status[f[0]] = f[1]
    return header, status


class Progress:
    """Follows a running piece's stdout (appended to) and keeps what the monitor shows."""

    def __init__(self, path, offset):
        self.path, self.offset = path, offset
        self.reply = None  # red: (rank, count, r2)
        self.total = self.earlier = self.refuted = self.open = 0
        self.phase = "ordering"

    def update(self):
        try:
            with open(self.path, "rb") as f:
                f.seek(self.offset)
                data = f.read()
        except FileNotFoundError:
            return
        end = data.rfind(b"\n")
        if end < 0:
            return
        self.offset += end + 1
        for line in data[: end + 1].decode("utf-8", "replace").splitlines():
            line = line.strip()
            if m := RE_REPLY.match(line):
                self.reply = (int(m.group(1)), int(m.group(2)), m.group(3))
                self.total = self.earlier = self.refuted = self.open = 0
                self.phase = "ordering"
            elif m := RE_ATTACKER.match(line):
                self.total, self.earlier = int(m.group(1)), int(m.group(2))
                self.refuted = self.open = 0
                self.phase = "answers first"
            elif m := RE_PASS1.match(line):
                self.phase = "pass 1"
                if m.group(4) == "refuted":
                    self.refuted += 1
                else:
                    self.open += 1
            elif m := RE_PASS2.match(line):
                self.phase = "pass 2"
                if m.group(2) == "refuted":
                    self.refuted += 1
                    self.open = max(0, self.open - 1)

    def done(self):
        return min(self.total, self.earlier + self.refuted) if self.total else 0


# ---- the solver binary -------------------------------------------------------------------------------


class Snek:
    def __init__(self, exe):
        self.exe = str(exe)

    def unique(self, moves, seconds=0, top=0, threads=1, mapping=()):
        """The side to move's unique moves best first: position, [(move, win probability, source)], {move: representative}."""
        cmd = [self.exe, "uniquemoves", str(seconds), str(top), str(threads), *moves]
        if mapping:
            cmd += ["--", *mapping]
        out = subprocess.run(cmd, capture_output=True, text=True, check=False,
                             creationflags=CREATE_NO_WINDOW if os.name == "nt" else 0).stdout
        position, ranked, mapped = None, [], {}
        for line in out.splitlines():
            f = line.split()
            if line.startswith("# position"):
                position = f[2]
            elif f and f[0] == "map" and len(f) == 3:
                mapped[f[1]] = None if f[2] == "illegal" else f[2]
            elif len(f) == 3 and f[2] in ("search", "eval"):
                ranked.append((f[0], float(f[1]), f[2]))
        if position is None:
            raise RuntimeError(f"{' '.join(cmd)}: {out.strip()[:200]}")
        return position, ranked, mapped


class ChildJob:
    """Windows job object that kills the solver processes when the driver exits, however it exits."""

    def __init__(self):
        self.job = None
        if os.name != "nt":
            return
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateJobObjectW.restype = ctypes.c_void_p
        k32.SetInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
        k32.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]

        class Basic(ctypes.Structure):
            _fields_ = [("PerProcessUserTimeLimit", ctypes.c_longlong), ("PerJobUserTimeLimit", ctypes.c_longlong),
                        ("LimitFlags", ctypes.c_uint32), ("MinimumWorkingSetSize", ctypes.c_size_t),
                        ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", ctypes.c_uint32),
                        ("Affinity", ctypes.c_size_t), ("PriorityClass", ctypes.c_uint32), ("SchedulingClass", ctypes.c_uint32)]

        class Extended(ctypes.Structure):
            _fields_ = [("Basic", Basic), ("IoInfo", ctypes.c_ulonglong * 6), ("ProcessMemoryLimit", ctypes.c_size_t),
                        ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                        ("PeakJobMemoryUsed", ctypes.c_size_t)]

        job = k32.CreateJobObjectW(None, None)
        info = Extended()
        info.Basic.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if job and k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)):  # extended limits
            self.job, self.k32 = job, k32

    def add(self, proc):
        if self.job and not self.k32.AssignProcessToJobObject(self.job, int(proc._handle)):
            return False
        return True


def pid_alive(pid):
    if os.name == "nt":
        k32 = ctypes.WinDLL("kernel32")
        k32.OpenProcess.restype = ctypes.c_void_p
        k32.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        k32.CloseHandle.argtypes = [ctypes.c_void_p]
        h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
        k32.CloseHandle(h)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


# ---- pieces ------------------------------------------------------------------------------------------


class Piece:
    def __init__(self, half, b1, r2=None, rank=0, rating=None, strong=False):
        self.half, self.b1, self.r2 = half, b1, r2
        self.id = f"R:{b1}" if half == "red" else f"B:{r2}"
        self.rank, self.rating, self.strong = rank, rating, strong
        self.status = "pending"
        self.hold = self.hold_rank = self.answer = None
        self.tried = 0
        self.sweep = False
        self.thread_seconds = 0.0
        self.source = None
        self.errors = 0
        self.before_run = "pending"  # the status before the running attempt
        self.ran_before = False  # an attempt was cut short (deferred, stopped or interrupted)

    @property
    def moves(self):
        return [CENTRE, self.b1] + ([self.r2] if self.r2 else [])

    def rel_dir(self):
        return f"pieces/R_{safe_name(self.b1)}" if self.half == "red" else f"pieces/B_{safe_name(self.b1)}"

    def rel_out(self):
        return f"{self.rel_dir()}.out" if self.half == "red" else f"{self.rel_dir()}/{safe_name(self.r2)}.out"

    def rel_log(self):  # blue: the refute log
        return f"{self.rel_dir()}/{safe_name(self.r2)}.log"

    def settled(self):
        return self.status in ("held", "blue-wins", "blue-loses")

    def evidence(self):
        """Where the result can be checked: the imported source, or this run's piece output."""
        return self.rel_out() if self.source == "run" else self.source or "-"

    def apply(self, rec):
        self.thread_seconds += rec.get("threadSeconds") or 0.0
        st = rec["status"]
        if st in ("stopped", "interrupted"):
            self.ran_before = True
            return
        if st == "error":
            self.errors += 1
            self.status = "error"
            return
        self.status = st
        self.source = rec.get("source", self.source)
        if st == "deferred":
            self.ran_before = True
        for key, attr in (("hold", "hold"), ("holdRank", "hold_rank"), ("tried", "tried"), ("answer", "answer"), ("sweep", "sweep")):
            if key in rec:
                setattr(self, attr, rec[key])


class Running:
    def __init__(self, piece, proc, threads, cap, out_file, offset, first_answers):
        self.piece, self.proc, self.threads, self.cap = piece, proc, threads, cap
        self.out_file = out_file
        self.started_wall = time.time()
        self.started = now_iso()
        self.progress = Progress(out_file, offset)
        self.first_answers = first_answers

    def thread_seconds(self):
        return (time.time() - self.started_wall) * self.threads


class Driver:
    def __init__(self, args):
        self.args = args
        self.run = Path(args.run).resolve()
        self.repo = Path(args.repo).resolve()
        self.snek = Snek(Path(args.snek).resolve())
        self.job = ChildJob()
        self.red, self.blue = {}, {}  # b1 -> Piece; (b1, r2) -> Piece
        self.red_order, self.blue_order = [], {}  # b1 hard first; b1 -> [r2 strongest first]
        self.running = {}  # piece id -> Running
        self.events, self.history = [], []
        self.stop_requested = False
        self.status = "starting"
        self.verdict = None  # (verdict, text) once known
        self.started = now_iso()
        self.last_state = self.last_history = 0.0
        self.dirty = True

    # -- files -------------------------------------------------------------------------------------
    def path(self, rel):
        return self.run / rel

    def event(self, kind, half, pid, text):
        rec = {"t": now_iso(), "kind": kind, "half": half, "id": pid, "text": text}
        append_jsonl(self.path("events.jsonl"), rec)
        self.events = (self.events + [rec])[-EVENTS_KEPT:]
        self.dirty = True
        print(f"{rec['t']} [{half}] {kind} {pid} {text}", flush=True)

    def record(self, piece, status, **fields):
        rec = {"t": now_iso(), "id": piece.id, "half": piece.half, "b1": piece.b1, "status": status}
        if piece.r2:
            rec["r2"] = piece.r2
        rec.update(fields)
        append_jsonl(self.path("pieces.jsonl"), rec)
        if status in ("stopped", "interrupted") and piece.status == "running":
            piece.status = piece.before_run  # cut short: back to pending or deferred
        piece.apply(rec)
        self.dirty = True

    # -- setup -------------------------------------------------------------------------------------
    def setup(self):
        setup_path = self.path("setup.json")
        if setup_path.exists():
            setup = json.loads(setup_path.read_text(encoding="utf-8"))
        else:
            setup = {"version": 1}
            ratings = {}
            src = self.repo / B1_RATINGS
            if src.exists():
                shutil.copyfile(src, self.path("b1_ratings.txt"))
                for line in src.read_text(encoding="utf-8").splitlines():
                    f = line.split()
                    if len(f) >= 3 and f[1] == "blue":
                        ratings[f[0]] = float(f[2])
            _, b1s, mapped = self.snek.unique([CENTRE], mapping=list(ratings))
            rating_of = {mapped[m]: r for m, r in ratings.items() if mapped.get(m)}
            order = sorted(range(len(b1s)), key=lambda i: (-(rating_of.get(b1s[i][0], -1.0)), i))  # hard first
            setup["red"] = [{"b1": b1s[i][0], "rating": rating_of.get(b1s[i][0])} for i in order]
            setup["blue"] = {}
            write_atomic(setup_path, json.dumps(setup, indent=1))
        self.setup_data = setup
        for i, line in enumerate(setup["red"]):
            self.red[line["b1"]] = Piece("red", line["b1"], rank=i + 1, rating=line["rating"])
            self.red_order.append(line["b1"])
        for b1 in setup["blue"]:
            self.add_blue_pieces(b1)

    def blue_setup(self, b1):
        """Red's replies after b1, strongest first (a 5 s search's top, then the evaluation), once per b1."""
        if b1 not in self.setup_data["blue"]:
            threads = max(1, min(self.args.threads, 8))
            _, ranked, _ = self.snek.unique([CENTRE, b1], self.args.blue_search, self.args.strong, threads)
            self.setup_data["blue"][b1] = [{"r2": m, "prob": p, "by": by} for m, p, by in ranked]
            write_atomic(self.path("setup.json"), json.dumps(self.setup_data, indent=1))
            self.add_blue_pieces(b1)
            self.import_blue(b1)

    def red_orders(self):
        """Red's replies in each open line, ordered once by a short search (one thread per process,
        all of the driver's threads): proveline takes the order from <line>/order.txt. A 5 s search
        completes only depth 3 here, and a search competing with running sweeps may stop at depth 2,
        which can move the reply that holds far down."""
        todo = [p for p in (self.red[b] for b in self.red_order)
                if not p.settled() and not (self.path(p.rel_dir()) / "order.txt").exists()]
        if not todo:
            return
        self.event("info", "red", "", f"ordering red's replies in {len(todo)} lines ({self.args.red_search} s searches)")
        depths, flags = {}, (BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW) if os.name == "nt" else 0
        while todo and not self.stop_requested and not self.path("STOP").exists():
            batch, todo = todo[: self.args.threads], todo[self.args.threads:]
            procs = []
            for p in batch:
                cmd = [self.snek.exe, "uniquemoves", str(self.args.red_search), "8", "1", *p.moves]
                proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL,
                                        text=True, creationflags=flags)
                self.job.add(proc)
                procs.append((p, proc))
            for p, proc in procs:
                out = proc.communicate()[0].splitlines()
                header = out[0].split() if out else []
                moves = [line.split()[0] for line in out[1:] if line.split() and line.split()[-1] in ("search", "eval")]
                if not moves or len(header) < 7:
                    continue  # proveline orders this line itself
                d = self.path(p.rel_dir())
                d.mkdir(parents=True, exist_ok=True)
                write_atomic(d / "order.txt", "".join(m + "\n" for m in moves))
                depths[int(header[6])] = depths.get(int(header[6]), 0) + 1
            self.write_state()
        self.event("info", "red", "", "search depths of the orders: " + ", ".join(f"depth {k}: {v} lines" for k, v in sorted(depths.items())))

    def add_blue_pieces(self, b1):
        order = []
        for i, r in enumerate(self.setup_data["blue"][b1]):
            self.blue[(b1, r["r2"])] = Piece("blue", b1, r["r2"], rank=i + 1, strong=i < self.args.strong)
            order.append(r["r2"])
        self.blue_order[b1] = order

    # -- imports -----------------------------------------------------------------------------------
    def imported_sources(self):
        return {(r["id"], r.get("b1"), r.get("source")) for r in read_jsonl(self.path("pieces.jsonl")) if r.get("source") != "run"}

    def import_record(self, piece, status, source, **fields):
        if (piece.id, piece.b1, source) in self._imported:
            return
        self._imported.add((piece.id, piece.b1, source))
        self.record(piece, status, source=source, **fields)

    def check_hold_log(self, moves, log, loss, answer=None):
        """'' if the refute log at `moves` proves the claim, else what is wrong: every move refuted
        (a hold, loss False), or `answer` holding (REFUTE_LOSS=1, loss True)."""
        path = self.repo / log
        if not path.exists():
            return f"{log} is missing"
        position, ranked, _ = self.snek.unique(moves)
        header, status = refute_log_status(path)
        want = f"# refute {position} loss {int(loss)} bonus 0"
        if header and header != want:
            return f"{log} starts with '{header}', not '{want}'"
        if answer is not None:
            return "" if status.get(answer) == "holds" else f"{log}: {answer} is not 'holds'"
        missing = [m for m, _, _ in ranked if status.get(m) != "refuted"]
        return f"{log}: {len(missing)} of {len(ranked)} blue moves not refuted" if missing else ""

    def import_red(self):
        for b1, summary, hold, log in RED_IMPORTS:
            piece = self.red.get(b1)
            if piece is None:
                self.event("error", "red", f"R:{b1}", "import: not a unique blue reply to 1:60")
                continue
            problem = self.check_hold_log([CENTRE, b1, hold], log, False)
            tried, rank, ts = 1, 1, None
            if summary:
                header, entries = parse_proveline_summary(self.repo / summary)
                position, _, _ = self.snek.unique([CENTRE, b1])
                if position not in header:
                    problem = problem or f"{summary} is not about 1:60 {b1}"
                holds = [i for i, e in enumerate(entries) if e[1] == "holds"]
                if not holds or entries[holds[0]][0] != hold:
                    problem = problem or f"{summary} does not end with {hold} holds"
                else:
                    tried = rank = holds[0] + 1
                times = []
                for line in (self.repo / summary).read_text(encoding="utf-8").splitlines():
                    m = re.search(r" (\d+) rank \d+ threads (\d+)$", line)
                    if m:
                        times.append(int(m.group(1)) * int(m.group(2)))
                ts = float(sum(times)) if times and len(times) == len(entries) else None
            if problem:
                self.event("error", "red", piece.id, "import refused: " + problem)
                continue
            self.import_record(piece, "held", summary or log, hold=hold, holdRank=rank, tried=tried, threadSeconds=ts)

    def import_blue(self, b1):
        if b1 not in BLUE_IMPORTS or b1 not in self.blue_order:
            return
        summary, draws = BLUE_IMPORTS[b1]
        claims = []  # (r2, status, answer, source, thread-seconds or None)
        header, entries = parse_proveline_summary(self.repo / summary)
        position, _, _ = self.snek.unique([CENTRE, b1])
        if position in header:
            claims += [(r2, "blue-wins", answer, summary, None) for r2, st, answer in entries if st == "wins"]
        else:
            self.event("error", "blue", "", f"import refused: {summary} is not about 1:60 {b1}")
        for r2, log in draws:
            claims.append((r2, "held", None, log, None))
        if b1 == BLUE_CANDIDATES[0]:
            claims += self.pilot_claims(b1)
        _, _, mapped = self.snek.unique([CENTRE, b1], mapping=sorted({c[0] for c in claims}))
        for r2, status, answer, source, ts in claims:
            rep = mapped.get(r2)
            piece = self.blue.get((b1, rep)) if rep else None
            if piece is None:
                self.event("error", "blue", f"B:{r2}", f"import refused: {r2} is not a red reply after 1:60 {b1}")
                continue
            if piece.settled():
                continue  # settled already (an earlier source wins)
            if status == "held" and answer is None:  # a draw log: its answer is the move that holds
                header, st = refute_log_status(self.repo / source)
                holding = [m for m, s in st.items() if s == "holds"]
                if len(holding) != 1:
                    self.event("error", "blue", piece.id, f"import refused: {source} has no single answer that holds")
                    continue
                answer = holding[0]
            if source.endswith(".log") or source.endswith(".out"):
                log = source if source.endswith(".log") else source[:-4] + ".log"
                problem = self.check_hold_log([CENTRE, b1, rep], log, True, answer)
                if problem:
                    self.event("error", "blue", piece.id, "import refused: " + problem)
                    continue
            self.import_record(piece, status, source, answer=answer, sweep=False, threadSeconds=ts)

    def pilot_claims(self, b1):
        claims = []
        costs = {}
        cost_file = self.repo / PILOT_COSTS
        if cost_file.exists():
            for line in cost_file.read_text(encoding="utf-8").splitlines():
                m = re.match(r"^\S+\s+(\S+)\s+proved-[AB]\s+(\S+)\s+cand \d+ th-s\s+([\d.]+)", line)
                if m:
                    costs[m.group(1)] = float(m.group(3))
        for rel, prefix in PILOT_STEP_DIRS:
            d = self.repo / rel
            if not d.is_dir():
                continue
            for log in sorted(d.glob(prefix + "*.log")):
                r2 = move_from_safe(log.stem[len(prefix):])
                _, st = refute_log_status(log)
                holding = [m for m, s in st.items() if s == "holds"]
                out = log.with_suffix(".out")
                step = re.findall(r"^STEP (proved-[AB]) answer (\S+)", out.read_text(encoding="utf-8", errors="replace"), re.M) if out.exists() else []
                if len(holding) != 1 or (step and step[-1][1] != holding[0]):
                    continue  # not settled (or the stdout disagrees): left to the run
                src = str(log.relative_to(self.repo)).replace("\\", "/")
                claims.append((r2, "held", holding[0], src, costs.get(r2)))
        hold_file = self.repo / PILOT_HOLD
        if hold_file.exists():
            block = None
            for line in hold_file.read_text(encoding="utf-8").splitlines():
                if line.startswith("== "):
                    f = line.split()
                    block = f[2:] if len(f) == 5 and f[2] == CENTRE and f[3] == b1 else None
                    proved = None
                elif block and (m := re.match(r"^\s+cand \d+ (\S+)\s+proves\s", line)):
                    proved = m.group(1)
                elif block and proved and (m := re.match(r"^STEP hold ply 3 proved cand \d+ threadsec ([\d.]+)", line)):
                    claims.append((block[2], "held", proved, PILOT_HOLD, float(m.group(1))))
                    block = None
        return claims

    # -- replay --------------------------------------------------------------------------------------
    def replay(self):
        for rec in read_jsonl(self.path("pieces.jsonl")):
            piece = self.red.get(rec["b1"]) if rec["half"] == "red" else self.blue.get((rec["b1"], rec.get("r2")))
            if piece is not None:
                piece.apply(rec)
        self.events = read_jsonl(self.path("events.jsonl"))[-EVENTS_KEPT:]
        self.history = read_jsonl(self.path("history.jsonl"))
        # attempts a crash cut short: their time, as the last state.json saw it
        old = self.path("state.json")
        if old.exists():
            try:
                state = json.loads(old.read_text(encoding="utf-8"))
                updated = dt.datetime.fromisoformat(state["run"]["updated"])
                for r in state.get("running", []):
                    piece = self.find(r["id"], r["half"], r["moves"])
                    if piece is not None:
                        secs = max(0.0, (updated - dt.datetime.fromisoformat(r["started"])).total_seconds()) * r["threads"]
                        self.record(piece, "interrupted", source="run", threadSeconds=secs)
            except (ValueError, KeyError, TypeError):
                pass

    def find(self, pid, half, moves):
        f = moves.split()
        if half == "red":
            return self.red.get(f[1]) if len(f) == 2 else None
        return self.blue.get((f[1], f[2])) if len(f) == 3 else None

    # -- blue's reply ------------------------------------------------------------------------------
    def blue_candidates(self):
        rest = [b1 for b1 in self.red_order if b1 not in BLUE_CANDIDATES]
        return BLUE_CANDIDATES + rest

    def b1_failed(self, b1):
        return any(p.status == "blue-loses" for (b, _), p in self.blue.items() if b == b1)

    def current_b1(self):
        for b1 in self.blue_candidates():
            if b1 not in self.blue_order or not self.b1_failed(b1):
                return b1
        return None

    def b1_history(self):
        out = []
        for b1 in self.blue_candidates():
            if b1 in self.blue_order and self.b1_failed(b1):
                loser = next(p for (b, _), p in self.blue.items() if b == b1 and p.status == "blue-loses")
                out.append({"b1": b1, "status": "failed", "note": f"loses to {loser.r2} ({loser.evidence()})"})
            else:
                out.append({"b1": b1, "status": "active", "note": "red's replies are being settled"})
                break
        return out

    # -- scheduling ----------------------------------------------------------------------------------
    def candidates(self, half):
        """Pieces to start next in this half, in priority order, with their caps (None: no cap)."""
        if half == "red":
            pieces = [self.red[b1] for b1 in self.red_order]
            cap = self.args.red_cap
        else:
            b1 = self.current_b1()
            if b1 is None or b1 not in self.blue_order:
                return []
            pieces = [self.blue[(b1, r2)] for r2 in self.blue_order[b1]]
            cap = self.args.blue_cap
        idle = [p for p in pieces if p.id not in self.running]
        pending = [(p, None if p.strong else cap) for p in idle if p.status == "pending"]
        if pending:
            return pending
        return [(p, None) for p in idle if p.status == "deferred" or (p.status == "error" and p.errors < MAX_ERRORS)]

    def schedule(self):
        if self.stop_requested or self.verdict:
            return
        per = self.args.proc_threads
        while True:
            free = self.args.threads - sum(r.threads for r in self.running.values())
            if free < 1:
                return
            busy = {"red": 0, "blue": 0}
            for r in self.running.values():
                busy[r.piece.half] += r.threads
            work = {h: self.candidates(h) for h in ("red", "blue")}
            red_target = round(self.args.threads * self.args.red_share)
            order = ["red", "blue"] if busy["red"] < red_target else ["blue", "red"]
            half = next((h for h in order if work[h]), None)
            if half is None:
                return
            piece, cap = work[half][0]
            self.launch(piece, min(per, free), cap)

    def launch(self, piece, threads, cap):
        out = self.path(piece.rel_out())
        out.parent.mkdir(parents=True, exist_ok=True)
        env = dict(os.environ)
        for k in ("REFUTE_LOSS", "REFUTE_FIRST", "SOLVER_RED_BONUS", "PROVELINE_ALL"):
            env.pop(k, None)
        first = []
        if piece.half == "red":
            d = self.path(piece.rel_dir())
            d.mkdir(parents=True, exist_ok=True)
            for f in list(d.glob("*.log")) + [d / "summary.txt"]:
                trim_partial_line(f)
            env["PROVELINE_SEARCH"] = str(self.args.red_search)
            cmd = [self.snek.exe, "proveline", str(threads), str(d), *piece.moves]
        else:
            trim_partial_line(self.path(piece.rel_log()))
            first = self.first_answers(piece.b1)
            write_atomic(self.path("blue_answers.txt"), "".join(a + "\n" for a in first))
            env["REFUTE_LOSS"] = "1"
            env["REFUTE_FIRST"] = str(self.path("blue_answers.txt"))
            cmd = [self.snek.exe, "refute", str(threads), str(self.path(piece.rel_log())), *piece.moves]
        with open(out, "ab") as f:
            f.write(f"\n{ATTEMPT_MARK}{now_iso()} threads {threads} cap {cap}\n".encode())
            offset = f.tell()
        handle = open(out, "ab")
        flags = (BELOW_NORMAL_PRIORITY_CLASS | CREATE_NO_WINDOW) if os.name == "nt" else 0
        proc = subprocess.Popen(cmd, stdout=handle, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, env=env,
                                creationflags=flags, cwd=str(self.run),
                                preexec_fn=(lambda: os.nice(10)) if os.name != "nt" else None)
        handle.close()
        if not self.job.add(proc):
            self.event("info", "run", piece.id, "could not tie the process to the driver's job")
        resumed = piece.ran_before or piece.status in ("deferred", "error")
        self.running[piece.id] = Running(piece, proc, threads, cap, out, offset, first)
        capped = f", first-pass cap {cap / 3600:.2f} thread-hours" if cap else ""
        self.event("resumed" if resumed else "start", piece.half, piece.id, f"{' '.join(piece.moves)} on {threads} threads{capped}")
        piece.before_run, piece.status = piece.status, "running"

    def first_answers(self, b1):
        """Blue's answers after b1 that held most often (then those that won most often)."""
        counts = {}
        for (b, _), p in self.blue.items():
            if b == b1 and p.status in ("held", "blue-wins") and p.answer:
                held, won = counts.get(p.answer, (0, 0))
                counts[p.answer] = (held + 1, won) if p.status == "held" else (held, won + 1)
        return sorted(counts, key=lambda a: (-counts[a][0], -counts[a][1], a))[: self.args.first_answers]

    # -- finished processes --------------------------------------------------------------------------
    def poll(self):
        for pid, r in list(self.running.items()):
            if self.running.get(pid) is not r:
                continue  # stopped meanwhile (blue's half moved to another reply)
            code = r.proc.poll()
            if code is None:
                if r.cap and r.thread_seconds() >= r.cap:
                    self.kill(r)
                    if r.proc.poll() is not None and self.finished(r, killed=True):
                        continue
                    del self.running[pid]
                    self.record(r.piece, "deferred", source="run", threadSeconds=r.thread_seconds())
                    self.event("deferred", r.piece.half, pid, f"past its first-pass cap of {r.cap / 3600:.2f} thread-hours; resumed later without a cap")
                continue
            self.finished(r)

    def kill(self, r):
        try:
            r.proc.terminate()
            r.proc.wait(timeout=30)
        except (OSError, subprocess.TimeoutExpired):
            pass

    def finished(self, r, killed=False):
        """Records a process that exited; False (nothing recorded) if it was killed before a verdict."""
        piece, secs = r.piece, r.thread_seconds()
        text = Path(r.out_file).read_text(encoding="utf-8", errors="replace")
        if piece.half == "blue":
            res = parse_refute_output(text)
            if killed and res["kind"] not in ("survives", "refuted-all"):
                return False
            del self.running[piece.id]
            if res["kind"] == "survives" and res["loss"]:
                sweep = res["answer"] not in r.first_answers
                self.record(piece, "held", source="run", answer=res["answer"], sweep=sweep, threadSeconds=secs, out=piece.rel_out())
                self.event("held", "blue", piece.id, f"blue does not lose with {res['answer']}" + (" (full sweep)" if sweep else ""))
            elif res["kind"] == "refuted-all" and res["loss"]:
                self.record(piece, "blue-loses", source="run", threadSeconds=secs, out=piece.rel_out())
                self.event("blue-loses", "blue", piece.id, f"blue LOSES after 1:60 {piece.b1} {piece.r2}: all {res['total']} blue moves refuted")
                self.switch_b1(piece)
            else:
                self.fail(piece, secs, res["line"] or f"exit code {r.proc.returncode}, no verdict")
        else:
            res = parse_proveline_output(text)
            if killed and res["kind"] not in ("held", "attacker-wins"):
                return False
            del self.running[piece.id]
            _, entries = parse_proveline_summary(self.path(piece.rel_dir()) / "summary.txt")
            order_file = self.path(piece.rel_dir()) / "order.txt"
            order = order_file.read_text(encoding="utf-8").split() if order_file.exists() else []
            if res["kind"] == "held":
                hold = next((e[0] for e in entries if e[1] == "holds"), None)
                rank = order.index(hold) + 1 if hold in order else len(entries)
                self.record(piece, "held", source="run", hold=hold, holdRank=rank, tried=len(entries), threadSeconds=secs, out=piece.rel_out())
                self.event("held", "red", piece.id, f"red holds with {hold} (reply {rank}; {len(entries) - 1} blue wins before it)")
            elif res["kind"] == "attacker-wins":
                self.record(piece, "blue-wins", source="run", tried=len(entries), threadSeconds=secs, out=piece.rel_out())
                self.event("blue-wins", "red", piece.id, f"blue WINS after 1:60 {piece.b1}: every red reply has a winning answer")
            else:
                self.fail(piece, secs, res["line"] or f"exit code {r.proc.returncode}, no verdict")
        return True

    def fail(self, piece, secs, why):
        self.record(piece, "error", source="run", threadSeconds=secs, note=why, out=piece.rel_out())
        again = " (tried again later)" if piece.errors < MAX_ERRORS else ""
        self.event("error", piece.half, piece.id, why + again)

    def switch_b1(self, loser):
        for pid, r in list(self.running.items()):
            if r.piece.half == "blue":
                self.kill(r)
                del self.running[pid]
                self.record(r.piece, "stopped", source="run", threadSeconds=r.thread_seconds())
        nxt = self.current_b1()
        if nxt is None:
            return
        self.event("info", "blue", "", f"blue's half moves from {loser.b1} (loses to {loser.r2}) to {nxt}")
        self.blue_setup(nxt)

    # -- verdict -------------------------------------------------------------------------------------
    def check_verdict(self):
        winner = next((p for p in self.red.values() if p.status == "blue-wins"), None)
        b1 = self.current_b1()
        red_done = all(p.status == "held" for p in self.red.values())
        blue_done = b1 in self.blue_order and all(self.blue[(b1, r2)].status in ("held", "blue-wins") for r2 in self.blue_order[b1])
        if winner:
            v = ("blue-wins", f"Blue wins against the centre opening: after 1:60 {winner.b1} every red reply has a blue answer that wins ({winner.evidence()}).")
        elif b1 is None:
            v = ("red-wins", "Red wins with the centre opening: every blue reply loses to some red reply (pieces.jsonl).")
        elif red_done and blue_done:
            v = ("draw", f"The centre opening is a draw: every one of the 172 blue replies has a red reply after which blue cannot win, "
                         f"and after blue's {b1} every red reply has a blue answer after which red cannot win.")
        else:
            return
        self.verdict = v
        self.event("verdict", "run", "", v[1])
        lines = [f"verdict: {v[0]}", v[1], "", "red's half (blue cannot win): b1, red's reply that holds, source"]
        lines += [f"  {p.b1} {p.status} {p.hold or '-'} {p.evidence()}" for p in (self.red[b] for b in self.red_order)]
        if b1:
            lines += ["", f"blue's half (red cannot win) after 1:60 {b1}: r2, status, blue's answer, source"]
            lines += [f"  {r2} {p.status} {p.answer or '-'} {p.evidence()}" for r2, p in ((r2, self.blue[(b1, r2)]) for r2 in self.blue_order[b1])]
        write_atomic(self.path("RESULT.txt"), "\n".join(lines) + "\n")

    # -- state.json ----------------------------------------------------------------------------------
    def write_state(self):
        t = time.time()
        for r in self.running.values():
            r.progress.update()
        b1 = self.current_b1()
        replies = [self.blue[(b1, r2)] for r2 in self.blue_order.get(b1, [])]
        pieces = [self.red[b] for b in self.red_order] + replies
        run_secs = {"red": [], "blue": []}
        for p in list(self.red.values()) + list(self.blue.values()):
            if p.source == "run" and p.settled():
                run_secs[p.half].append(p.thread_seconds)
        partial = sum(r.thread_seconds() for r in self.running.values())
        total_secs = sum(p.thread_seconds for p in list(self.red.values()) + list(self.blue.values())) + partial
        done = sum(1 for p in pieces if p.status in ("held", "blue-wins"))
        remaining = 0.0
        for half, group in (("red", [self.red[b] for b in self.red_order]), ("blue", replies)):
            mean = sum(run_secs[half]) / len(run_secs[half]) if run_secs[half] else PRIOR_THREAD_SECONDS[half]
            remaining += sum(max(0.0, mean - (self.running[p.id].thread_seconds() if p.id in self.running else 0.0))
                             for p in group if not p.settled())
        busy = sum(r.threads for r in self.running.values())
        verdict, text = self.verdict or ("open", f"Open: red's half {sum(p.status == 'held' for p in self.red.values())} of {len(self.red)} lines held, "
                                                 f"blue's half {sum(p.status in ('held', 'blue-wins') for p in replies)} of {len(replies)} red replies after {b1} settled.")
        if t - self.last_history >= HISTORY_SECONDS and self.status == "running":
            point = {"t": now_iso(), "threadSeconds": round(total_secs, 1), "piecesDone": done, "busyThreads": busy}
            append_jsonl(self.path("history.jsonl"), point)
            self.history.append(point)
            self.last_history = t
        state = {
            "version": 1,
            "run": {"name": "centre", "root": CENTRE, "status": self.status, "started": self.started, "updated": now_iso(),
                    "binary": self.snek.exe, "binarySha256": self.sha, "threads": self.args.threads, "busyThreads": busy},
            "summary": {"threadSeconds": round(total_secs, 1), "piecesTotal": len(pieces), "piecesDone": done,
                        "etaSeconds": None if self.verdict else round(remaining / max(1, self.args.threads), 0),
                        "verdict": verdict, "verdictText": text},
            "red": {"goal": "blue cannot win: for every blue reply to 1:60, a red reply after which every blue move is refuted",
                    "lines": [self.red_line(self.red[b]) for b in self.red_order]},
            "blue": {"goal": f"red cannot win: after blue's {b1}, a blue answer to every red reply after which red cannot win",
                     "b1": b1, "b1History": self.b1_history(), "replies": [self.blue_reply(p) for p in replies]},
            "running": [self.running_entry(r) for r in self.running.values()],
            "events": self.events,
            "history": self.history,
        }
        try:
            write_atomic(self.path("state.json"), json.dumps(state, separators=(",", ":")))
        except OSError as e:  # a reader held the file the whole time: the next pass writes it
            print(f"{now_iso()} state.json not written: {e}", file=sys.stderr, flush=True)
            return
        self.last_state, self.dirty = t, False

    def red_line(self, p):
        r = self.running.get(p.id)
        current = r.progress.reply[2] if r and r.progress.reply else None
        out = p.rel_out() if (r or self.path(p.rel_out()).exists()) else None
        return {"id": p.id, "b1": p.b1, "rating": p.rating, "status": p.status, "hold": p.hold, "holdRank": p.hold_rank,
                "tried": p.tried if not r else self.red_tried(p), "threadSeconds": round(p.thread_seconds + (r.thread_seconds() if r else 0), 1),
                "source": p.source or "run", "current": current, "out": out}

    def red_tried(self, p):
        _, entries = parse_proveline_summary(self.path(p.rel_dir()) / "summary.txt")
        return len(entries)

    def blue_reply(self, p):
        r = self.running.get(p.id)
        out = p.rel_out() if (r or p.source == "run" or p.ran_before or p.errors) else None
        return {"id": p.id, "r2": p.r2, "rank": p.rank, "status": p.status, "answer": p.answer,
                "threadSeconds": round(p.thread_seconds + (r.thread_seconds() if r else 0), 1), "sweep": bool(p.sweep),
                "source": p.source or "run", "out": out}

    def running_entry(self, r):
        g = r.progress
        detail = f"{g.refuted} refuted, {g.open} open" if g.total else "ordering the moves"
        if r.piece.half == "red" and g.reply:
            detail = f"red reply {g.reply[0]} {g.reply[2]}: blue's moves {detail}"
        return {"id": r.piece.id, "half": r.piece.half, "moves": " ".join(r.piece.moves), "threads": r.threads, "started": r.started,
                "phase": g.phase, "done": g.done(), "total": g.total, "detail": detail, "out": r.piece.rel_out()}

    # -- main loop -----------------------------------------------------------------------------------
    def step(self):
        """One pass of the loop: finished processes, the verdict, new processes, state.json.
        False when the loop should end."""
        self.poll()
        self.check_verdict()
        if self.verdict:
            return False
        self.schedule()
        if not self.running and not any(self.candidates(h) for h in ("red", "blue")):
            failed = sum(1 for p in list(self.red.values()) + list(self.blue.values()) if p.status == "error")
            self.event("info", "run", "", f"nothing left to run: {failed} pieces failed {MAX_ERRORS} times (see the errors above)")
            return False
        if self.dirty or time.time() - self.last_state >= STATE_SECONDS:
            self.write_state()
        return True

    def request_stop(self, *_):
        self.stop_requested = True

    def main(self):
        self.run.mkdir(parents=True, exist_ok=True)
        lock = self.path("driver.pid")
        if lock.exists():
            try:
                other = int(lock.read_text().split()[0])
            except (ValueError, IndexError):
                other = 0
            if other and other != os.getpid() and pid_alive(other):
                print(f"a driver (pid {other}) already runs on {self.run}; stop it with --stop", file=sys.stderr)
                return 1
        lock.write_text(f"{os.getpid()}\n")
        self.path("STOP").unlink(missing_ok=True)
        for sig in ("SIGINT", "SIGTERM", "SIGBREAK"):
            if hasattr(signal, sig):
                signal.signal(getattr(signal, sig), self.request_stop)
        if os.name == "nt":
            ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
        try:
            self.sha = hashlib.sha256(Path(self.snek.exe).read_bytes()).hexdigest()
            try:
                self.snek.unique([CENTRE])
            except RuntimeError as e:
                print(f"{self.snek.exe} cannot list moves (a build with 'snek uniquemoves' is needed): {e}", file=sys.stderr)
                return 1
            self.setup()
            self.replay()
            self._imported = self.imported_sources()
            self.event("info", "run", "", f"driver started: {self.args.threads} threads, {self.snek.exe}")
            self.import_red()
            for b1 in BLUE_CANDIDATES:
                if b1 in self.setup_data["blue"]:
                    self.import_blue(b1)
            b1 = self.current_b1()
            if b1 is not None:
                self.blue_setup(b1)
            self.red_orders()
            self.status = "running"
            self.check_verdict()
            failures = 0
            while not self.stop_requested and not self.verdict:
                if self.path("STOP").exists():
                    self.stop_requested = True
                    break
                try:
                    if not self.step():
                        break
                    failures = 0
                except Exception as e:  # a multi-day run outlives one bad pass: report it and go on
                    failures += 1
                    traceback.print_exc()
                    self.event("error", "run", "", f"driver loop: {e!r} ({failures} in a row)")
                    if failures >= MAX_LOOP_FAILURES:
                        break
                    time.sleep(5.0)
                time.sleep(1.0)
            self.status = "stopping"
            self.write_state()
            for pid, r in list(self.running.items()):
                self.kill(r)
                if not self.finished(r, killed=True):
                    del self.running[pid]
                    self.record(r.piece, "stopped", source="run", threadSeconds=r.thread_seconds())
            self.status = "done" if self.verdict else "stopped"
            self.event("info", "run", "", f"driver {self.status}")
            self.write_state()
        finally:
            if os.name == "nt":
                ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS)
            lock.unlink(missing_ok=True)
            self.path("STOP").unlink(missing_ok=True)
        return 0


def stop(run):
    run = Path(run).resolve()
    lock = run / "driver.pid"
    if not lock.exists():
        print(f"no driver runs on {run}")
        return 0
    try:
        pid = int(lock.read_text().split()[0])
    except (ValueError, IndexError):
        pid = 0
    if not pid_alive(pid):
        lock.unlink(missing_ok=True)
        print(f"no driver runs on {run} (removed the stale driver.pid of process {pid})")
        return 0
    (run / "STOP").write_text("stop\n")
    for _ in range(120):
        if not lock.exists():
            print("stopped")
            return 0
        time.sleep(1)
    print("the driver has not stopped yet (it stops its solver processes first)", file=sys.stderr)
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--snek", help="the solver binary (snek.exe)")
    ap.add_argument("--run", required=True, help="the run directory (created; a rerun resumes it)")
    ap.add_argument("--threads", type=int, default=8, help="threads for all solver processes together")
    ap.add_argument("--stop", action="store_true", help="stop the driver running on --run")
    ap.add_argument("--proc-threads", type=int, default=2, help="threads per solver process")
    ap.add_argument("--red-share", type=float, default=0.4, help="share of the threads for red's half while both have work")
    ap.add_argument("--red-cap", type=float, default=3 * 3600, help="first-pass cap of a red line, thread-seconds")
    ap.add_argument("--blue-cap", type=float, default=1800, help="first-pass cap of a blue step past red's strongest replies, thread-seconds")
    ap.add_argument("--strong", type=int, default=50, help="red's strongest replies by the search, run first and without a cap")
    ap.add_argument("--blue-search", type=float, default=5, help="seconds of the search that ranks red's replies")
    ap.add_argument("--red-search", type=int, default=5, help="PROVELINE_SEARCH seconds for red's lines")
    ap.add_argument("--first-answers", type=int, default=3, help="blue answers that held most often, tried first in a step")
    ap.add_argument("--repo", default=str(REPO), help="the repository whose results/ and build/ hold earlier proofs")
    args = ap.parse_args()
    if args.stop:
        return stop(args.run)
    if not args.snek:
        ap.error("--snek is required to run")
    return Driver(args).main()


if __name__ == "__main__":
    sys.exit(main())
