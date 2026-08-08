"""HTTP 流媒体服务器: 把 ffmpeg 的 stdout 输出以 MPEG-TS (chunked) 流式提供给 DLNA 播放设备。"""

import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PAGE = """<!doctype html><html lang="zh"><head><meta charset="utf-8">
<title>DLNA-Caster</title></head><body>
<h1>DLNA-Caster</h1>
<p>流地址: <a href="/stream">http://本机IP:端口/stream</a></p>
<p>如果电视没有自动播放, 可以用 VLC 打开上面的地址预览。</p>
</body></html>"""


class StreamHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "DLNA-Caster/1.0"

    def do_GET(self):
        if self.path.rstrip("/") == "/stream":
            self._stream()
        else:
            body = PAGE.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def _stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "video/mp2t")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Cache-Control", "no-cache, no-store")
        self.end_headers()
        pipe = self.server.pipe
        try:
            while True:
                with self.server.pipe_lock:
                    chunk = pipe.read(65536)
                if not chunk:
                    break
                self.wfile.write(f"{len(chunk):x}\r\n".encode("ascii") + chunk + b"\r\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError,
                TimeoutError, OSError):
            pass
        finally:
            try:
                self.wfile.write(b"0\r\n\r\n")
            except Exception:
                pass

    def log_message(self, fmt, *args):
        pass


class StreamServer(ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, addr, pipe=None):
        self.pipe = pipe
        self.pipe_lock = threading.Lock()
        super().__init__(addr, StreamHandler)
