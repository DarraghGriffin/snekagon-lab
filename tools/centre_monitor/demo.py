"""Fake centre-opening run for developing the monitor: writes a run directory by the contract
(tools/centre_monitor/CONTRACT.md) and, with --advance, keeps it moving like a live run.

    python tools/centre_monitor/demo.py --run build/monitor/demo [--advance] [--speed 40] [--alarm] [--seed 1]

The positions are random placements of real piece shapes (not necessarily legal moves); the settled lines
known from results/ are imported with their real moves. Ctrl+C stops --advance and leaves the run 'stopped'.
"""
import argparse
import json
import os
import random
import time
from datetime import datetime, timedelta, timezone

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHAPES = {
    '1': [(0, 0)], '2': [(0, 0), (1, 0)], '3I': [(0, 0), (0, 1), (0, 2)], '3V': [(0, 0), (1, 0), (1, 1)],
    '4I': [(0, 0), (-1, 1), (-2, 2), (-3, 3)], '4A': [(0, 0), (1, 0), (1, 1), (1, 2)],
    '4B': [(0, 0), (0, 1), (1, 1), (1, 2)], '4C': [(0, 0), (-1, 1), (-1, 2), (0, 2)],
}
CENTRE = '1:60'
BLUE_B1 = '4B:16,27,28,39'
RED_LINES = 172
BLUE_REPLIES = 1589
THREADS = 14
HISTORY_STEP = 300
TICK_SECONDS = 2.0
# Red's settled lines (README 'Centre opening: what is proven'): blue's reply, red's hold, its rank, source.
RED_IMPORTED = [
    ('4B:16,27,28,39', '4C:40,41,50,52', 30, 'results/centre_4B_proveline.txt'),
    ('4A:51,52,63,74', '3V:29,40,50', 17, 'results/centre_4A51_proveline.txt'),
    ('3V:36,37,48', '4A:58,59,68,78', 20, 'results/centre_3V36_proveline.txt'),
    ('4C:26,36,47,48', '4A:58,59,68,78', 2, 'results/centre_4C26_line.txt'),
    ('4A:18,19,30,41', '4A:42,50,51,52', 1, 'results/hold_4A18_4A42_refute.log'),
    ('3V:55,56,67', '4C:71,73,82,83', 1, 'results/hold_3V55_4C71_refute.log'),
    ('3V:35,36,47', '4A:18,28,38,49', 1, 'results/hold_3V35_4A18_refute.log'),
]


def on_board(q, r):
    return abs(q) <= 5 and abs(r) <= 5 and abs(q + r) <= 5


def cell(q, r):
    return (r + 5) * 11 + (q + 5)


def orientations(shape):
    out, seen = [], set()
    for mirror in (False, True):
        pts = [(r, q) if mirror else (q, r) for q, r in shape]
        for _ in range(6):
            pts = [(-r, q + r) for q, r in pts]
            q0, r0 = min(pts, key=lambda p: (p[1], p[0]))
            key = tuple(sorted((q - q0, r - r0) for q, r in pts))
            if key not in seen:
                seen.add(key)
                out.append(key)
    return out


PLACEMENTS = {}
for _piece, _shape in SHAPES.items():
    PLACEMENTS[_piece] = []
    for _form in orientations(_shape):
        for _q in range(-5, 6):
            for _r in range(-5, 6):
                cells = [(_q + dq, _r + dr) for dq, dr in _form]
                if all(on_board(q, r) for q, r in cells):
                    PLACEMENTS[_piece].append(sorted(cell(q, r) for q, r in cells))


def move_cells(move):
    return {int(c) for c in move.split(':')[1].split(',')} if ':' in move else set()


def random_move(rng, taken, pieces=('2', '3I', '3V', '4I', '4A', '4B', '4C')):
    while True:
        piece = rng.choice(pieces)
        cells = rng.choice(PLACEMENTS[piece])
        if not taken.intersection(cells):
            return f'{piece}:' + ','.join(map(str, cells))


def distinct_moves(rng, taken, count, fixed=()):
    out, seen = list(fixed), set(fixed)
    while len(out) < count:
        move = random_move(rng, taken)
        if move not in seen:
            seen.add(move)
            out.append(move)
    return out


def iso(t):
    return t.astimezone().isoformat(timespec='seconds')


def write_json_atomic(path, value):
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(value, f, separators=(',', ':'))
    for attempt in range(20):
        try:
            os.replace(tmp, path)
            return
        except PermissionError:  # another process has it open (Windows)
            time.sleep(0.05 * (attempt + 1))
    os.replace(tmp, path)


def read_proveline(path):
    """(red reply, status, blue answer) of each settled reply in a proveline summary."""
    out = []
    try:
        with open(path) as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 3 and parts[1] == 'wins':
                    out.append((parts[0], 'blue-wins', parts[2]))
    except OSError:
        pass
    return out


class Demo:
    def __init__(self, run_dir, seed, alarm, speed):
        self.run_dir = run_dir
        self.rng = random.Random(seed)
        self.speed = speed
        self.pieces_dir = os.path.join(run_dir, 'pieces')
        os.makedirs(self.pieces_dir, exist_ok=True)
        now = datetime.now(timezone.utc)
        self.started = now - timedelta(hours=7, minutes=12)
        self.events = []
        self.history = []
        self.running = {}
        rng = self.rng

        b1s = distinct_moves(rng, {60}, RED_LINES, [b1 for b1, *_ in RED_IMPORTED])
        rng.shuffle(b1s)
        self.red = []
        imported = {b1: (hold, rank, src) for b1, hold, rank, src in RED_IMPORTED}
        for b1 in b1s:
            rating = round(min(0.97, max(0.02, rng.betavariate(2, 5))), 3) if rng.random() > 0.03 else None
            line = {'id': f'R:{b1}', 'b1': b1, 'rating': rating, 'status': 'pending', 'hold': None,
                    'holdRank': None, 'tried': 0, 'threadSeconds': 0.0, 'source': 'run', 'current': None, 'out': None}
            if b1 in imported:
                hold, rank, src = imported[b1]
                line.update(status='held', hold=hold, holdRank=rank, tried=rank, source=src,
                            threadSeconds=round(rank * rng.uniform(2000, 9000), 1))
            self.red.append(line)

        taken = {60} | move_cells(BLUE_B1)
        known = read_proveline(os.path.join(REPO, 'results', 'centre_4B_proveline.txt'))
        fixed = [r2 for r2, *_ in known] + ['4C:40,41,50,52']
        r2s = distinct_moves(rng, taken, BLUE_REPLIES, fixed)
        head, tail = r2s[:len(fixed)], r2s[len(fixed):]
        rng.shuffle(head)
        r2s = head[:12] + tail[:60] + head[12:] + tail[60:]
        known_map = {r2: (status, answer) for r2, status, answer in known}
        known_map['4C:40,41,50,52'] = ('held', '4A:49,59,70,81')
        self.blue = []
        for rank, r2 in enumerate(r2s, 1):
            reply = {'id': f'B:{r2}', 'r2': r2, 'rank': rank, 'status': 'pending', 'answer': None,
                     'threadSeconds': 0.0, 'sweep': False, 'source': 'run', 'out': None}
            if r2 in known_map:
                status, answer = known_map[r2]
                reply.update(status=status, answer=answer, threadSeconds=round(rng.uniform(150, 900), 1),
                             sweep=status == 'held',
                             source='results/hold_4C40_draw_refute.log' if status == 'held'
                             else 'results/centre_4B_proveline.txt')
            self.blue.append(reply)

        # The run so far: about 7 hours of 14 threads settled the first lines of each half.
        for line in sorted(self.red, key=lambda x: -(x['rating'] or 0))[:26]:
            if line['status'] == 'pending':
                self.settle_red(line)
        for reply in self.blue[:330]:
            if reply['status'] == 'pending':
                self.settle_blue(reply)
        deferred = rng.sample(self.blue[:330], 3)
        for reply in deferred:
            reply.update(status='deferred', answer=None, sweep=False)
        if alarm:
            self.blue[201].update(status='blue-loses', answer=None, sweep=True)
            line = next(x for x in self.red if x['status'] == 'pending')
            line.update(status='blue-wins', tried=rng.randint(30, 60), threadSeconds=round(rng.uniform(4e4, 9e4), 1))
            self.event('blue-loses', 'blue', self.blue[201]['id'], f"{self.blue[201]['r2']}: every blue answer loses")
            self.event('blue-wins', 'red', line['id'], f"{line['b1']}: every red reply loses")
        settled = sorted(rng.uniform(0, 1) for _ in range(len(self.events)))
        span = (now - self.started).total_seconds()
        for event, frac in zip(self.events, settled):
            event['t'] = iso(self.started + timedelta(seconds=span * frac))
        self.events = [{'t': iso(self.started), 'kind': 'start', 'half': 'run', 'id': '',
                        'text': f'run started: {THREADS} threads, root {CENTRE}'}] + self.events[-299:]
        total = self.done_seconds()
        for k in range(int(span // HISTORY_STEP) + 1):
            frac = (k * HISTORY_STEP / span) ** 0.95
            self.history.append({'t': iso(self.started + timedelta(seconds=k * HISTORY_STEP)),
                                 'threadSeconds': round(total * frac, 1),
                                 'piecesDone': int(self.pieces_done() * frac),
                                 'busyThreads': THREADS if k else 0})
        self.next_history = self.started + timedelta(seconds=(len(self.history)) * HISTORY_STEP)
        while len(self.running) < 4:
            self.start_next()

    # ------------------------------------------------------------------ pieces

    def out_path(self, piece_id):
        name = piece_id.replace(':', '_').replace(',', '-') + '.out'
        return 'pieces/' + name

    def append_out(self, rel, lines):
        with open(os.path.join(self.run_dir, *rel.split('/')), 'a', encoding='utf-8') as f:
            f.write(''.join(line + '\n' for line in lines))

    def event(self, kind, half, piece_id, text):
        self.events.append({'t': iso(datetime.now(timezone.utc)), 'kind': kind, 'half': half,
                            'id': piece_id, 'text': text})
        self.events = self.events[-300:]

    def settle_red(self, line):
        rng = self.rng
        hard = line['rating'] or 0.3
        if rng.random() < 0.04:
            line.update(status='deferred', current=None)
            self.event('deferred', 'red', line['id'], f"{line['b1']}: deferred after {line['tried']} red replies")
            return
        rank = line['tried'] if line['tried'] else max(1, int(rng.expovariate(1 / (1 + 6 * hard))))
        hold = random_move(rng, {60} | move_cells(line['b1']))
        line.update(status='held', hold=hold, holdRank=rank, tried=rank, current=None,
                    threadSeconds=round(line['threadSeconds'] or rank * rng.uniform(1500, 7000), 1))
        self.event('held', 'red', line['id'], f"{line['b1']}: red holds with {hold} (rank {rank})")

    def settle_blue(self, reply):
        rng = self.rng
        taken = {60} | move_cells(BLUE_B1) | move_cells(reply['r2'])
        answer = random_move(rng, taken)
        held = rng.random() < 0.18
        reply.update(status='held' if held else 'blue-wins', answer=answer, sweep=held and rng.random() < 0.6,
                     threadSeconds=round(reply['threadSeconds'] or rng.lognormvariate(5.4, 0.6), 1))
        verb = 'holds' if held else 'wins'
        self.event('held' if held else 'blue-wins', 'blue', reply['id'],
                   f"{reply['r2']}: blue {verb} with {answer}" + (' (full sweep)' if reply['sweep'] else ''))

    def start_next(self):
        busy = sum(p['threads'] for p in self.running.values())
        threads = min(4 if len(self.running) < 2 else 3, THREADS - busy)
        if threads <= 0:
            return
        red_running = sum(1 for p in self.running.values() if p['half'] == 'red')
        if red_running < 2:
            line = next((x for x in sorted(self.red, key=lambda x: -(x['rating'] or 0)) if x['status'] == 'pending'), None)
            if line:
                self.start_red(line, threads)
                return
        reply = next((x for x in self.blue if x['status'] == 'pending'), None)
        if reply:
            self.start_blue(reply, threads)

    def start_red(self, line, threads):
        rng = self.rng
        line.update(status='running', tried=0, out=self.out_path(line['id']))
        current = random_move(rng, {60} | move_cells(line['b1']))
        line['current'] = current
        total = rng.randint(850, 1100)
        self.running[line['id']] = {
            'id': line['id'], 'half': 'red', 'moves': f"{CENTRE} {line['b1']} {current}", 'threads': threads,
            'started': iso(datetime.now(timezone.utc)), 'phase': 'pass 1', 'done': 0, 'total': total,
            'detail': f"red reply 1: {current}", 'out': line['out'], '_rank': 1,
            '_holdAt': max(1, int(rng.expovariate(1 / (1 + 5 * (line['rating'] or 0.3))))), '_progress': 0.0}
        self.append_out(line['out'], [f"# proveline {CENTRE} {line['b1']}", f"red reply 1 {current}: refute ({total} blue moves)"])
        self.event('start', 'red', line['id'], f"{line['b1']}: proveline started on {threads} threads")

    def start_blue(self, reply, threads):
        rng = self.rng
        reply.update(status='running', out=self.out_path(reply['id']))
        total = rng.randint(600, 1050)
        self.running[reply['id']] = {
            'id': reply['id'], 'half': 'blue', 'moves': f"{CENTRE} {BLUE_B1} {reply['r2']}", 'threads': threads,
            'started': iso(datetime.now(timezone.utc)), 'phase': 'candidates', 'done': 0, 'total': total,
            'detail': 'proving earlier answers first', 'out': reply['out'], '_progress': 0.0}
        self.append_out(reply['out'], [f"# REFUTE_LOSS=1 refute {CENTRE} {BLUE_B1} {reply['r2']}",
                                       'candidates: 9 answers that held earlier'])

    def tick(self, dt):
        rng = self.rng
        for piece_id, piece in list(self.running.items()):
            rate = piece['threads'] * self.speed * rng.uniform(0.4, 1.6) / (8.0 if piece['half'] == 'red' else 1.5)
            piece['_progress'] += rate * dt
            step = int(piece['_progress'])
            piece['_progress'] -= step
            if step <= 0:
                continue
            piece['done'] = min(piece['total'], piece['done'] + step)
            taken = {60} | {c for m in piece['moves'].split() for c in move_cells(m)}
            lines = [f"{random_move(rng, taken)} refuted {random_move(rng, taken)} {rng.randint(10**5, 10**7)} "
                     f"{rng.uniform(0.1, 20):.1f}" for _ in range(min(step, 12))]
            self.append_out(piece['out'], lines)
            if piece['half'] == 'blue' and piece['phase'] == 'candidates' and piece['done'] > piece['total'] * 0.1:
                piece.update(phase='pass 1', detail='sweep of the remaining blue answers')
            if piece['half'] == 'red' and piece['done'] > piece['total'] * 0.6:
                piece['phase'] = 'pass 2'
            if piece['done'] >= piece['total']:
                self.finish(piece_id, piece)
        while sum(p['threads'] for p in self.running.values()) < THREADS - 2 and self.start_next_possible():
            self.start_next()

    def start_next_possible(self):
        return any(x['status'] == 'pending' for x in self.blue)

    def finish(self, piece_id, piece):
        rng = self.rng
        elapsed = (datetime.now(timezone.utc) - datetime.fromisoformat(piece['started'])).total_seconds()
        cost = max(30.0, elapsed * piece['threads'] * self.speed)
        if piece['half'] == 'red':
            line = next(x for x in self.red if x['id'] == piece_id)
            line['tried'] = piece['_rank']
            line['threadSeconds'] = round(line['threadSeconds'] + cost, 1)
            if piece['_rank'] >= piece['_holdAt']:
                self.append_out(piece['out'], [f"red reply {piece['_rank']} {line['current']}: holds"])
                line['hold'] = line['current']
                del self.running[piece_id]
                self.settle_red(line)
                return
            self.append_out(piece['out'], [f"red reply {piece['_rank']} {line['current']}: blue wins"])
            piece['_rank'] += 1
            current = random_move(rng, {60} | move_cells(line['b1']))
            line['current'] = current
            piece.update(moves=f"{CENTRE} {line['b1']} {current}", phase='pass 1', done=0,
                         total=rng.randint(850, 1100), detail=f"red reply {piece['_rank']}: {current}")
            return
        reply = next(x for x in self.blue if x['id'] == piece_id)
        reply['threadSeconds'] = round(cost, 1)
        del self.running[piece_id]
        self.settle_blue(reply)

    # ------------------------------------------------------------------ state

    def done_seconds(self):
        return sum(x['threadSeconds'] for x in self.red + self.blue)

    def pieces_done(self):
        return (sum(1 for x in self.red if x['status'] in ('held', 'blue-wins'))
                + sum(1 for x in self.blue if x['status'] in ('held', 'blue-wins', 'blue-loses')))

    def state(self, status='running'):
        now = datetime.now(timezone.utc)
        if now >= self.next_history:
            self.history.append({'t': iso(now), 'threadSeconds': round(self.done_seconds(), 1),
                                 'piecesDone': self.pieces_done(),
                                 'busyThreads': sum(p['threads'] for p in self.running.values())})
            self.next_history = now + timedelta(seconds=HISTORY_STEP)
        done = self.pieces_done()
        total = len(self.red) + len(self.blue)
        rate = self.done_seconds() / max(1, done)
        eta = (total - done) * rate / THREADS / 2.4 if status == 'running' else None
        loses = sum(1 for x in self.blue if x['status'] == 'blue-loses')
        verdict_text = (f"Open: red holds {sum(1 for x in self.red if x['status'] == 'held')} of {len(self.red)} "
                        f"blue replies; blue holds {done - loses - sum(1 for x in self.red if x['status'] == 'held')} "
                        f"of red's {len(self.blue)} replies.")
        if loses:
            verdict_text += f" {loses} red reply beats blue's {BLUE_B1}: blue needs another reply."
        busy = sum(p['threads'] for p in self.running.values())
        return {
            'version': 1,
            'run': {'name': 'centre', 'root': CENTRE, 'status': status, 'started': iso(self.started),
                    'updated': iso(now), 'binary': 'build/centre/snek_centre.exe',
                    'binarySha256': '9f2c4e1a7b3d5f60' * 4, 'threads': THREADS, 'busyThreads': busy},
            'summary': {'threadSeconds': round(self.done_seconds(), 1), 'piecesTotal': total, 'piecesDone': done,
                        'etaSeconds': round(eta, 0) if eta else None, 'verdict': 'open', 'verdictText': verdict_text},
            'red': {'goal': "Blue cannot win: for each of blue's 172 replies to 1:60, a red reply after which "
                            "every blue move is refuted.", 'lines': self.red},
            'blue': {'goal': f"Red cannot win: after blue's {BLUE_B1}, each of red's replies has a blue answer "
                             "after which red cannot win.", 'b1': BLUE_B1,
                     'b1History': [{'b1': BLUE_B1, 'status': 'active',
                                    'note': "engine's first choice; the U-block 4C:40,41,50,52 holds it to a draw"}],
                     'replies': self.blue},
            'running': [{k: v for k, v in p.items() if not k.startswith('_')} for p in self.running.values()],
            'events': self.events,
            'history': self.history,
        }

    def write(self, status='running'):
        write_json_atomic(os.path.join(self.run_dir, 'state.json'), self.state(status))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--run', required=True, help='run directory to create or overwrite')
    ap.add_argument('--advance', action='store_true', help='keep the run moving until Ctrl+C')
    ap.add_argument('--speed', type=float, default=40.0, help='work rate multiplier while advancing')
    ap.add_argument('--alarm', action='store_true', help='include a result in each half that breaks the proof')
    ap.add_argument('--seed', type=int, default=1)
    args = ap.parse_args()
    demo = Demo(args.run, args.seed, args.alarm, args.speed)
    demo.write()
    print(f'demo run written to {os.path.abspath(args.run)}', flush=True)
    if not args.advance:
        return
    try:
        last = time.monotonic()
        while True:
            time.sleep(TICK_SECONDS)
            now = time.monotonic()
            demo.tick(now - last)
            last = now
            demo.write()
    except KeyboardInterrupt:
        for piece in demo.running.values():
            demo.event('info', piece['half'], piece['id'], 'stopped; resumes from its log')
        demo.running.clear()
        demo.event('info', 'run', '', 'run stopped')
        demo.write('stopped')


if __name__ == '__main__':
    main()
