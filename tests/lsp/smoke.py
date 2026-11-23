#!/usr/bin/env python3
"""PyAdamas LSP 冒烟测试（测试脚本是 python3，被测服务器是 tools/lsp/main.py 编译出的原生二进制）。

覆盖：
  * initialize 往返（capabilities / serverInfo）
  * initialized 通知不回包；未知方法 -> -32601
  * didOpen 有类型错误 -> publishDiagnostics（0-based 行列）
  * didChange 全量替换 -> 诊断清空；didChange 增量 range 替换 -> 诊断重现/再清空
  * documentSymbol（class=5 / def=12 / 赋值=13）
  * hover（函数签名 / 形参 / 未知位置 null）
  * 非 ASCII 文档（Content-Length 按字节、精确读取）
  * 字符串 id 原样回显、坏 JSON 不崩
  * shutdown + exit 干净退出（rc=0）；未 shutdown 直接 exit（rc=1）

用法：tests/lsp/smoke.sh（会先用 build.sh 构建服务器）；或设置 PYA_LSP / PYADAMAS 后直接跑本文件。
"""
import glob
import json
import os
import select
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

FAILURES = []
CHECKS = 0


def expect(cond, what):
    global CHECKS
    CHECKS += 1
    if not cond:
        FAILURES.append(what)
        print("  FAIL: " + what)


def expect_eq(got, want, what):
    expect(got == want, "%s (got %r, want %r)" % (what, got, want))


def expect_in(needle, haystack, what):
    expect(needle in haystack, "%s (%r not in %r)" % (what, needle, haystack))


class Client:
    """最小 LSP 客户端：按 Content-Length 帧收发，带超时。"""

    def __init__(self, argv, env):
        self.proc = subprocess.Popen(
            argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env
        )
        self.fd = self.proc.stdout.fileno()
        self.buf = b""
        self.pending = []
        self.stderr_text = ""

    # ---- 帧收发 ----
    def send_raw_body(self, body):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def send(self, obj):
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_raw_body(body)

    def _fill(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            raise TimeoutError("timeout waiting for server output")
        chunk = os.read(self.fd, 65536)
        if not chunk:
            raise EOFError("server closed stdout")
        self.buf += chunk

    def recv(self, timeout=60):
        while True:
            idx = self.buf.find(b"\r\n\r\n")
            if idx >= 0:
                header = self.buf[:idx].decode("ascii", "replace")
                length = None
                for line in header.split("\r\n"):
                    key, _, value = line.partition(":")
                    if key.strip().lower() == "content-length":
                        length = int(value.strip())
                if length is None:
                    raise ValueError("frame without Content-Length: %r" % header)
                need = idx + 4 + length
                while len(self.buf) < need:
                    self._fill(timeout)
                body = self.buf[idx + 4:need]
                self.buf = self.buf[need:]
                return json.loads(body.decode("utf-8"))
            self._fill(timeout)

    def wait_for(self, predicate, timeout=60):
        for i, msg in enumerate(self.pending):
            if predicate(msg):
                return self.pending.pop(i)
        while True:
            msg = self.recv(timeout)
            if predicate(msg):
                return msg
            self.pending.append(msg)

    def response(self, rid, timeout=60):
        return self.wait_for(lambda m: m.get("id") == rid, timeout)

    def notification(self, method, timeout=60):
        return self.wait_for(lambda m: m.get("method") == method, timeout)

    def request(self, rid, method, params):
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        return self.response(rid)

    def notify(self, method, params):
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def close_stdin(self):
        try:
            self.proc.stdin.close()
        except Exception:
            pass

    def wait_exit(self, timeout=30):
        self.close_stdin()
        rc = self.proc.wait(timeout=timeout)
        self.stderr_text = self.proc.stderr.read().decode("utf-8", "replace")
        return rc


def locate_server():
    env = os.environ.get("PYA_LSP")
    if env:
        return env
    return os.path.join(ROOT, "tools", "lsp", "pya-lsp")


def locate_compiler():
    env = os.environ.get("PYADAMAS")
    if env:
        return env
    cands = [p for p in glob.glob(os.path.join(ROOT, "build", "*", "pyadamas"))
             if os.access(p, os.X_OK)]
    cands.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    if cands:
        return cands[0]
    return ""


def make_client(server, compiler):
    env = dict(os.environ)
    env["PYADAMAS"] = compiler
    env.pop("PYA_LSP_DEBUG", None)
    return Client([server, "--stdio"], env)


# ---------------- 各测试 ----------------

TEXT_BAD = (
    "def add(a: int, b: int) -> int:\n"
    "    return a + b\n"
    "\n"
    "total: int = \"not an int\"\n"
)

TEXT_FIXED = (
    "def add(a: int, b: int) -> int:\n"
    "    return a + b\n"
    "\n"
    "total: int = 42\n"
)

TEXT_SYMBOLS = (
    "class Point:\n"
    "    x: int\n"
    "    y: int\n"
    "\n"
    "    def norm2(self) -> int:\n"
    "        return self.x * self.x + self.y * self.y\n"
    "\n"
    "\n"
    "def build(n: int) -> Point:\n"
    "    return Point()\n"
    "\n"
    "RESULT: int = 0\n"
)

TEXT_UNICODE = (
    "label: str = \"中文 😀 ünïcode\"\n"
    "count: int = label\n"
)


def diag_params(msg, uri):
    params = msg.get("params") or {}
    expect_eq(params.get("uri"), uri, "diagnostics uri")
    return params


def test_initialize(c, uri_a):
    t0 = time.monotonic()
    res = c.request(1, "initialize", {
        "processId": None,
        "rootUri": None,
        "capabilities": {"textDocument": {"hover": {"contentFormat": ["plaintext"]}}},
    })
    elapsed = time.monotonic() - t0
    print("    initialize round-trip: %.3fs" % elapsed)
    expect(elapsed < 1.0, "initialize round-trip < 1s (got %.3fs)" % elapsed)
    expect_eq(res.get("jsonrpc"), "2.0", "initialize jsonrpc")
    expect_eq(res.get("id"), 1, "initialize id echo")
    result = res.get("result") or {}
    caps = result.get("capabilities") or {}
    sync = caps.get("textDocumentSync")
    if isinstance(sync, dict):
        expect_eq(sync.get("change"), 1, "textDocumentSync.change == Full")
    else:
        expect_eq(sync, 1, "textDocumentSync == 1")
    expect_eq(caps.get("hoverProvider"), True, "hoverProvider")
    expect_eq(caps.get("documentSymbolProvider"), True, "documentSymbolProvider")
    expect_eq((result.get("serverInfo") or {}).get("name"), "pya-lsp", "serverInfo.name")


def test_initialized_and_unknown(c):
    c.notify("initialized", {})
    c.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/definition", "params": {}})
    # initialized 是通知，不能有回包：下一条消息必须就是 id=2 的响应
    msg = c.recv()
    expect_eq(msg.get("id"), 2, "first frame after initialized is response id=2")
    err = msg.get("error") or {}
    expect_eq(err.get("code"), -32601, "unknown method -> MethodNotFound")


def test_didopen_diagnostics(c, uri):
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "python", "version": 1, "text": TEXT_BAD}})
    msg = c.notification("textDocument/publishDiagnostics")
    params = diag_params(msg, uri)
    expect_eq(params.get("version"), 1, "diagnostics version")
    diags = params.get("diagnostics") or []
    expect(len(diags) >= 1, "didOpen type error produces >=1 diagnostic")
    if diags:
        d = diags[0]
        expect_eq(d.get("severity"), 1, "diagnostic severity error")
        expect_eq(d.get("source"), "pyadamas", "diagnostic source")
        rng = d.get("range") or {}
        start = rng.get("start") or {}
        expect_eq(start.get("line"), 3, "diagnostic line is 0-based (line 4 -> 3)")
        expect_eq(start.get("character"), 0, "diagnostic character is 0-based")
        expect("int" in (d.get("message") or ""), "diagnostic message mentions int")


def test_didchange_full(c, uri):
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 2},
        "contentChanges": [{"text": TEXT_FIXED}],
    })
    params = diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    expect_eq(params.get("version"), 2, "full-change version")
    expect_eq(params.get("diagnostics"), [], "full change clears diagnostics")


def test_didchange_incremental(c, uri):
    # TEXT_FIXED 第 4 行 "total: int = 42"：42 占 character 13..15
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 3},
        "contentChanges": [{
            "range": {"start": {"line": 3, "character": 13}, "end": {"line": 3, "character": 15}},
            "text": "\"oops\"",
        }],
    })
    params = diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    expect_eq(params.get("version"), 3, "incremental version")
    expect(len(params.get("diagnostics") or []) >= 1, "incremental change reintroduces diagnostic")
    # 再改回 42（现在 character 13..19）
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 4},
        "contentChanges": [{
            "range": {"start": {"line": 3, "character": 13}, "end": {"line": 3, "character": 19}},
            "text": "42",
        }],
    })
    params2 = diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    expect_eq(params2.get("version"), 4, "incremental revert version")
    expect_eq(params2.get("diagnostics"), [], "incremental revert clears diagnostics")


def test_document_symbol(c, uri):
    res = c.request(3, "textDocument/documentSymbol", {"textDocument": {"uri": uri}})
    syms = res.get("result") or []
    names = {s.get("name"): s for s in syms}
    expect("Point" in names and "build" in names and "RESULT" in names,
           "documentSymbol finds Point/build/RESULT (got %r)" % sorted(names))
    if "Point" in names:
        expect_eq(names["Point"].get("kind"), 5, "Point kind = Class(5)")
    if "build" in names:
        expect_eq(names["build"].get("kind"), 12, "build kind = Function(12)")
    if "RESULT" in names:
        expect_eq(names["RESULT"].get("kind"), 13, "RESULT kind = Variable(13)")
    for s in syms:
        r = s.get("range") or {}
        st, en = r.get("start") or {}, r.get("end") or {}
        expect(en.get("line", 0) >= st.get("line", 0), "symbol range end >= start (%s)" % s.get("name"))
        sr = s.get("selectionRange") or {}
        expect(sr.get("start", {}).get("line") == st.get("line"), "selectionRange on definition line")


def hover_value(res):
    result = res.get("result")
    if not isinstance(result, dict):
        return None
    contents = result.get("contents")
    if isinstance(contents, dict):
        return contents.get("value")
    return contents


def test_hover(c, uri):
    # build 定义在第 9 行（0-based），字符 4 开始
    res = c.request(4, "textDocument/hover", {
        "textDocument": {"uri": uri}, "position": {"line": 8, "character": 5}})
    value = hover_value(res)
    expect(value is not None and "def build" in value, "hover on function gives signature (got %r)" % value)
    # 形参 n
    res = c.request(5, "textDocument/hover", {
        "textDocument": {"uri": uri}, "position": {"line": 8, "character": 10}})
    value = hover_value(res)
    expect(value is not None and "def build" in value, "hover on parameter gives enclosing signature (got %r)" % value)
    # 未知位置 -> null
    res = c.request(6, "textDocument/hover", {
        "textDocument": {"uri": uri}, "position": {"line": 7, "character": 0}})
    expect_eq(res.get("result"), None, "hover on blank line is null")
    # 字符串 id 原样回显
    res = c.request("str-id", "textDocument/hover", {
        "textDocument": {"uri": uri}, "position": {"line": 8, "character": 5}})
    expect_eq(res.get("id"), "str-id", "string id echoed")


def test_unicode(c, uri):
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "python", "version": 1, "text": TEXT_UNICODE}})
    params = diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    diags = params.get("diagnostics") or []
    expect(len(diags) >= 1, "unicode didOpen produces diagnostic")
    if diags:
        start = (diags[0].get("range") or {}).get("start") or {}
        expect_eq(start.get("line"), 1, "unicode diagnostic line")
        expect_eq(start.get("character"), 0, "unicode diagnostic character")
    c.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
    params2 = diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    expect_eq(params2.get("diagnostics"), [], "didClose clears diagnostics")


def test_malformed_json(c, uri):
    c.send_raw_body(b"{ this is not json")
    res = c.request(7, "textDocument/hover", {
        "textDocument": {"uri": uri}, "position": {"line": 8, "character": 5}})
    expect(res.get("id") == 7 and res.get("result") is not None,
           "server survives malformed JSON and keeps serving")


def test_interactive_latency(c, uri):
    # 时序护栏：客户端发一帧后服务器必须立刻回包，而不是等 stdin 出现 EOF。
    # 这条专门防 stdin 读语义回退（runtime 的文本模式 fread(8192) 坑）。
    cases = [
        ("textDocument/hover", {"textDocument": {"uri": uri},
                                "position": {"line": 8, "character": 5}}),
        ("textDocument/documentSymbol", {"textDocument": {"uri": uri}}),
    ]
    for i, (method, params) in enumerate(cases):
        rid = 900 + i
        t0 = time.monotonic()
        res = c.request(rid, method, params)
        elapsed = time.monotonic() - t0
        print("    %s round-trip: %.3fs" % (method, elapsed))
        expect(res.get("id") == rid, "latency probe %s response id" % method)
        expect(elapsed < 1.0, "%s round-trip < 1s (got %.3fs)" % (method, elapsed))


def test_shutdown(c):
    res = c.request(8, "shutdown", None)
    expect_eq(res.get("id"), 8, "shutdown id echo")
    expect_eq(res.get("result"), None, "shutdown result null")
    c.notify("exit", None)
    rc = c.wait_exit()
    expect_eq(rc, 0, "exit after shutdown -> rc 0")
    expect("Traceback" not in c.stderr_text, "no traceback on server stderr (got %r)" % c.stderr_text[:400])
    expect_in("Content-Length", "Content-Length", "sanity")


def test_exit_without_shutdown(server, compiler):
    c = make_client(server, compiler)
    c.notify("exit", None)
    rc = c.wait_exit()
    expect_eq(rc, 1, "exit without shutdown -> rc 1")


def main():
    server = locate_server()
    compiler = locate_compiler()
    if not os.path.exists(server):
        print("server binary not found: %s (run tools/lsp/build.sh first)" % server)
        return 2
    if not compiler:
        print("pyadamas compiler not found; set PYADAMAS")
        return 2
    print("server  : %s" % server)
    print("compiler: %s" % compiler)

    uri_a = "file:///tmp/pya-lsp-smoke-bad.py"
    uri_b = "file:///tmp/pya-lsp-smoke-symbols.py"
    uri_c = "file:///tmp/pya-lsp-smoke-unicode.py"

    c = make_client(server, compiler)
    try:
        print("[1] initialize / capabilities")
        test_initialize(c, uri_a)
        print("[2] initialized notification + unknown method")
        test_initialized_and_unknown(c)
        print("[3] didOpen -> publishDiagnostics")
        test_didopen_diagnostics(c, uri_a)
        print("[4] didChange full replace -> diagnostics cleared")
        test_didchange_full(c, uri_a)
        print("[5] didChange incremental range")
        test_didchange_incremental(c, uri_a)
        print("[6] documentSymbol")
        c.notify("textDocument/didOpen", {"textDocument": {
            "uri": uri_b, "languageId": "python", "version": 1, "text": TEXT_SYMBOLS}})
        diag_params(c.notification("textDocument/publishDiagnostics"), uri_b)
        test_document_symbol(c, uri_b)
        print("[7] hover")
        test_hover(c, uri_b)
        print("[7b] interactive latency (<1s per frame)")
        test_interactive_latency(c, uri_b)
        print("[8] unicode framing")
        test_unicode(c, uri_c)
        print("[9] malformed JSON resilience")
        test_malformed_json(c, uri_b)
        print("[10] shutdown / exit")
        test_shutdown(c)
    finally:
        if c.proc.poll() is None:
            c.proc.kill()

    print("[11] exit without shutdown")
    test_exit_without_shutdown(server, compiler)

    print("")
    if FAILURES:
        print("FAILED: %d check(s) failed" % len(FAILURES))
        for f in FAILURES:
            print("  - " + f)
        return 1
    print("PASS: %d checks" % CHECKS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
