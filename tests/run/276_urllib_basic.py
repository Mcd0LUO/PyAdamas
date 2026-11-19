# 276 urllib.request 基础面（libcurl 原生）：本地 http.server 子进程做源，
# GET/POST/自定义头（Request）/重定向/HEAD/二进制体/分块 read/上下文管理。
# 对拍 CPython urllib.request（同一本地 URL，只钉功能输出）。端口 18326。
import subprocess
import time
import os
import tempfile
import shutil
from urllib.request import urlopen, Request
from urllib.error import URLError

SRV = '''
import http.server
import time

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="text/plain", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        if extra:
            for k, v in extra:
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/hello":
            self._send(200, b"Hello, World!", extra=[("X-Test", "stdlib2")])
        elif self.path == "/head":
            v = self.headers.get("X-Custom", "")
            self._send(200, ("X-Custom: " + v + chr(10)).encode())
        elif self.path == "/redir":
            self.send_response(302)
            self.send_header("Location", "/hello")
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path == "/bytes":
            self._send(200, bytes(range(256)), ctype="application/octet-stream")
        else:
            self._send(404, b"nope")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(n)
        ct = self.headers.get("Content-Type", "")
        self._send(200, data, extra=[("X-Method", "POST"), ("X-Ct", ct)])

    def do_HEAD(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", "13")
        self.end_headers()

http.server.HTTPServer(("127.0.0.1", 18326), H).serve_forever()
'''

BASE = "http://127.0.0.1:18326"
d = tempfile.mkdtemp(prefix="pa276_")
srvpath = d + "/srv.py"
fs = open(srvpath, "w")
fs.write(SRV)
fs.close()
srv = subprocess.Popen(["python3", srvpath], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    ready = False
    i = 0
    while i < 100:
        try:
            r = urlopen(BASE + "/hello")
            r.read()
            r.close()
            ready = True
            break
        except URLError:
            time.sleep(0.05)
            i += 1
    print("ready:", ready)

    with urlopen(BASE + "/hello") as r:
        print("status:", r.status, r.getcode())
        print("body:", r.read())
        print("ctype:", r.headers.get("Content-Type"))
        print("xtest:", r.headers.get("X-Test"))
        print("missing:", r.headers.get("X-Nope") is None)
        print("geturl:", r.geturl() == BASE + "/hello")
        print("contains:", "content-type" in r.headers)
        print("getitem:", r.headers["X-Test"])
    print("closed after with:", r.closed)

    with urlopen(BASE + "/hello") as r:
        print("chunk:", r.read(5), r.read())

    with urlopen(Request(BASE + "/head", headers={"X-Custom": "abc123"})) as r:
        print("hdr echo:", r.read())

    with urlopen(BASE + "/echo", data=b"a=1&b=2") as r:
        print("post body:", r.read())
        print("post method:", r.headers.get("X-Method"))
        print("post ct:", r.headers.get("X-Ct"))

    with urlopen(Request(BASE + "/echo", data=b"\x00\x01\xff", headers={"Content-Type": "application/octet-stream"})) as r:
        print("post bin:", r.read())
        print("post bin ct:", r.headers.get("X-Ct"))

    with urlopen(BASE + "/redir") as r:
        print("redir status:", r.status)
        print("redir body:", r.read())
        print("redir url:", r.geturl() == BASE + "/hello")

    with urlopen(BASE + "/bytes") as r:
        bb: bytes = r.read()
        print("bytes len:", len(bb), bb[0], bb[255])

    with urlopen(Request(BASE + "/hello", method="HEAD")) as r:
        print("head status:", r.status)
        print("head body:", r.read())
        print("head clen:", r.headers.get("Content-Length"))
    print("done")
finally:
    srv.kill()
    srv.wait()
    shutil.rmtree(d, ignore_errors=True)

# --- expect ---
# ready: True
# status: 200 200
# body: b'Hello, World!'
# ctype: text/plain
# xtest: stdlib2
# missing: True
# geturl: True
# contains: True
# getitem: stdlib2
# closed after with: True
# chunk: b'Hello' b', World!'
# hdr echo: b'X-Custom: abc123\n'
# post body: b'a=1&b=2'
# post method: POST
# post ct: application/x-www-form-urlencoded
# post bin: b'\x00\x01\xff'
# post bin ct: application/octet-stream
# redir status: 200
# redir body: b'Hello, World!'
# redir url: True
# bytes len: 256 0 255
# head status: 200
# head body: b''
# head clen: 13
# done
