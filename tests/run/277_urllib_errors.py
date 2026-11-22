# 277 urllib.request 错误面（libcurl 原生）：HTTPError（404/500，带响应面）、
# 传输阶段超时 TimeoutError、非法 URL ValueError/URLError、连接拒绝 URLError、
# 307 GET 跟随。本地 http.server 子进程，端口 18327。对拍 CPython。
import os
import subprocess
import time
import tempfile
import shutil
from urllib.request import urlopen
from urllib.error import URLError, HTTPError

SRV = '''
import http.server
import sys
import time

class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body):
        self.send_response(code)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/hello":
            self._send(200, b"Hello, World!")
        elif self.path == "/404":
            self._send(404, b"not found")
        elif self.path == "/500":
            self._send(500, b"boom")
        elif self.path == "/slow":
            time.sleep(2)
            self._send(200, b"ok")
        elif self.path == "/redir307":
            self.send_response(307)
            self.send_header("Location", "/hello")
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self._send(404, b"nope")

http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
'''

# 端口按进程号散列：并发跑 difftest 时不会互相抢固定端口（原本硬编码 18327，
# 两个 difftest 并行会有一个起不来，退出码不同 → 假失败）。
PORT = 18000 + (os.getpid() % 2000)
BASE = "http://127.0.0.1:" + str(PORT)
d = tempfile.mkdtemp(prefix="pa277_")
srvpath = d + "/srv.py"
fs = open(srvpath, "w")
fs.write(SRV)
fs.close()
srv = subprocess.Popen(["python3", srvpath, str(PORT)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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

    try:
        urlopen(BASE + "/404")
    except HTTPError as e:
        print("404 code:", e.code, e.status)
        print("404 str:", str(e))
        print("404 body:", e.read())
        print("404 ctype:", e.headers.get("Content-Type"))
        print("404 url:", e.geturl() == BASE + "/404")
        print("404 getcode:", e.getcode())
    try:
        urlopen(BASE + "/500")
    except HTTPError as e:
        print("500 code:", e.code)
        print("500 body:", e.read())

    try:
        urlopen(BASE + "/slow", timeout=0.3)
    except TimeoutError as e:
        print("timeout:", str(e))

    try:
        urlopen("not a url")
    except ValueError:
        print("valueerror ok")
    try:
        urlopen("gopher://127.0.0.1/x")
    except URLError as e:
        print("scheme:", str(e))
    try:
        urlopen("http://127.0.0.1:1/x")
    except URLError:
        print("conn refused ok")

    with urlopen(BASE + "/redir307") as r:
        print("307 body:", r.read())
        print("307 url:", r.geturl() == BASE + "/hello")
    print("done")
finally:
    srv.kill()
    srv.wait()
    shutil.rmtree(d, ignore_errors=True)

# --- expect ---
# ready: True
# 404 code: 404 404
# 404 str: HTTP Error 404: Not Found
# 404 body: b'not found'
# 404 ctype: text/plain
# 404 url: True
# 404 getcode: 404
# 500 code: 500
# 500 body: b'boom'
# timeout: timed out
# valueerror ok
# scheme: <urlopen error unknown url type: gopher>
# conn refused ok
# 307 body: b'Hello, World!'
# 307 url: True
# done
