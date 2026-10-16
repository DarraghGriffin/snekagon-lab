"""Local web monitor of the centre-opening run: serves web/ and the run directory's state.

    python tools/centre_monitor/server.py --run <run dir> [--port 8770] [--access-log]

The run driver owns the run directory (the contract is in tools/centre_monitor/CONTRACT.md): it rewrites
<run>/state.json atomically and keeps each piece's stdout under <run>/pieces/. The server only reads:

    GET /                 the dashboard (web/index.html and its files)
    GET /api/state        state.json, re-read only when its mtime or size changes (ETag, 304 when unchanged);
                          {"waiting": true} until the run directory has a state.json
    GET /api/tail?file=<path relative to the run dir>&lines=<n>
                          the last n lines (at most 400) of a regular file inside the run directory

It binds 127.0.0.1 only and does no other work per request.
"""
import argparse
import hashlib
import json
import os
import sys
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

WEB_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'web')
DEFAULT_PORT = 8770
STATE_FILE = 'state.json'
TAIL_DEFAULT_LINES = 60
TAIL_MAX_LINES = 400
TAIL_MAX_BYTES = 256 * 1024
STATIC_TYPES = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.svg': 'image/svg+xml',
    '.ico': 'image/x-icon',
}


def read_bytes(path, offset=0, length=-1):
    """One read with the file open as briefly as possible: on Windows the driver's os.replace of
    state.json fails (PermissionError, retried by the writer) while any reader has it open."""
    with open(path, 'rb') as f:
        if offset:
            f.seek(offset)
        return f.read(length)


class StateCache:
    """The last good copy of state.json; re-read only when the file's mtime or size changes."""

    def __init__(self, run_dir):
        self.path = os.path.join(run_dir, STATE_FILE)
        self.lock = threading.Lock()
        self.stamp = None
        self.body = None
        self.etag = None

    def get(self):
        """(body, etag) of the last good copy, (None, None) before there is one. A file that cannot be
        read for the moment (Windows refuses opens while the driver replaces it), or that is torn,
        keeps the previous copy."""
        try:
            st = os.stat(self.path)
        except OSError:
            return self.body, self.etag
        stamp = (st.st_mtime_ns, st.st_size)
        with self.lock:
            if stamp != self.stamp:
                try:
                    body = read_bytes(self.path)
                    json.loads(body)
                except (OSError, ValueError) as err:
                    if self.body is None:
                        print(f'state.json not readable yet: {err}', file=sys.stderr)
                    return self.body, self.etag
                self.stamp = stamp
                self.body = body
                self.etag = '"' + hashlib.blake2b(body, digest_size=12).hexdigest() + '"'
            return self.body, self.etag


def tail_lines(path, count):
    size = os.path.getsize(path)
    start = max(0, size - TAIL_MAX_BYTES)
    data = read_bytes(path, start, size - start)
    text = data.decode('utf-8', errors='replace')
    lines = text.splitlines()
    if start > 0 and lines:
        lines = lines[1:]  # the first line is cut
    return size, lines[-count:]


class Handler(BaseHTTPRequestHandler):
    server_version = 'CentreMonitor/1'
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        if self.server.access_log:
            super().log_message(fmt, *args)

    def send_body(self, status, body, content_type, headers=None):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-cache')
        self.send_header('X-Content-Type-Options', 'nosniff')
        for key, value in (headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def send_json(self, status, value, headers=None):
        body = json.dumps(value, separators=(',', ':')).encode()
        self.send_body(status, body, 'application/json', headers)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        url = urlsplit(self.path)
        if url.path == '/api/state':
            self.serve_state()
        elif url.path == '/api/tail':
            self.serve_tail(parse_qs(url.query))
        else:
            self.serve_static(url.path)

    def serve_state(self):
        body, etag = self.server.state.get()
        if body is None:
            self.send_json(HTTPStatus.OK, {'waiting': True, 'runDir': self.server.run_dir},
                           {'ETag': '"waiting"'})
        elif self.headers.get('If-None-Match') == etag:
            self.send_response(HTTPStatus.NOT_MODIFIED)
            self.send_header('ETag', etag)
            self.send_header('Content-Length', '0')
            self.end_headers()
        else:
            self.send_body(HTTPStatus.OK, body, 'application/json', {'ETag': etag})

    def serve_tail(self, query):
        relative = (query.get('file') or [''])[0]
        try:
            count = int((query.get('lines') or [TAIL_DEFAULT_LINES])[0])
        except ValueError:
            count = TAIL_DEFAULT_LINES
        count = max(1, min(TAIL_MAX_LINES, count))
        path = self.server.run_file(relative)
        if path is None:
            self.send_json(HTTPStatus.FORBIDDEN, {'error': 'only files inside the run directory'})
            return
        try:
            size, lines = tail_lines(path, count)
        except PermissionError:
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE, {'error': 'busy, try again', 'file': relative})
            return
        except OSError:
            self.send_json(HTTPStatus.NOT_FOUND, {'error': 'no such file yet', 'file': relative})
            return
        self.send_json(HTTPStatus.OK, {'file': relative, 'size': size, 'lines': lines})

    def serve_static(self, path):
        name = 'index.html' if path in ('', '/') else path.lstrip('/')
        ext = os.path.splitext(name)[1].lower()
        if ext not in STATIC_TYPES or '..' in name.split('/') or '\\' in name:
            self.send_json(HTTPStatus.NOT_FOUND, {'error': 'not found'})
            return
        full = os.path.join(WEB_DIR, *name.split('/'))
        try:
            with open(full, 'rb') as f:
                body = f.read()
        except OSError:
            self.send_json(HTTPStatus.NOT_FOUND, {'error': 'not found'})
            return
        self.send_body(HTTPStatus.OK, body, STATIC_TYPES[ext])


class MonitorServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, run_dir, access_log):
        super().__init__(address, Handler)
        self.run_dir = os.path.abspath(run_dir)
        self.state = StateCache(self.run_dir)
        self.access_log = access_log

    def handle_error(self, request, client_address):
        if isinstance(sys.exc_info()[1], ConnectionError):
            return  # the browser closed the connection mid-response
        super().handle_error(request, client_address)

    def run_file(self, relative):
        """The real path of a regular file named relative to the run directory, or None if it is
        absolute, escapes the directory (.., links) or is not a plain relative path."""
        if not relative or len(relative) > 512 or '\0' in relative:
            return None
        if os.path.isabs(relative) or os.path.splitdrive(relative)[0] or relative.startswith(('/', '\\')):
            return None
        root = os.path.realpath(self.run_dir)
        path = os.path.realpath(os.path.join(root, relative))
        try:
            if os.path.commonpath([root, path]) != root or path == root:
                return None
        except ValueError:
            return None
        if os.path.exists(path) and not os.path.isfile(path):
            return None
        return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--run', required=True, help='run directory (it may not exist yet)')
    ap.add_argument('--port', type=int, default=DEFAULT_PORT)
    ap.add_argument('--access-log', action='store_true', help='print one line per request')
    args = ap.parse_args()
    server = MonitorServer(('127.0.0.1', args.port), args.run, args.access_log)
    print(f'centre monitor: http://127.0.0.1:{args.port}/  (run dir {server.run_dir})', flush=True)
    try:
        server.serve_forever(poll_interval=1.0)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == '__main__':
    main()
