#!/usr/bin/env python3
"""PyAdamas LSP 冒烟测试（测试脚本是 python3，被测服务器是 tools/lsp/main.py 编译出的原生二进制）。

覆盖：
  * initialize 往返（capabilities / serverInfo）
  * initialized 通知不回包；未知方法 -> -32601
  * didOpen 有类型错误 -> publishDiagnostics（0-based 行列）
  * didChange 全量替换 -> 诊断清空；didChange 增量 range 替换 -> 诊断重现/再清空
  * documentSymbol（class=5 / def=12 / 赋值=13）
  * hover（函数签名 / 形参 / 未知位置 null）
  * completion（文档符号 / import 成员 / 关键字；前缀过滤）
  * definition（跳到 def/class/赋值定义；未知符号 null）
  * semanticTokens/full（非 ASCII 文档的 UTF-16 行列/长度编码）
  * signatureHelp（activeParameter 数逗号 / 无参函数 / 字符串里的逗号 / UTF-16 列）
  * documentHighlight（同名出现 / 注释与字符串里的同名不算 / UTF-16 列）
  * foldingRange（def/class 块、连续注释块、跨行三引号字符串块）
  * didChange debounce（连发两次只发布最终版本的诊断）
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
    expect_eq(caps.get("definitionProvider"), True, "definitionProvider")
    comp = caps.get("completionProvider") or {}
    expect(isinstance(comp, dict), "completionProvider is an object")
    expect_eq(comp.get("triggerCharacters"), ["."], "completion triggerCharacters")
    sem = caps.get("semanticTokensProvider") or {}
    expect_eq(sem.get("full"), True, "semanticTokensProvider.full")
    legend = sem.get("legend") or {}
    expect_eq(legend.get("tokenTypes"),
              ["class", "function", "variable", "keyword", "string", "number",
               "comment", "parameter"], "semanticTokens legend tokenTypes")
    expect_eq((result.get("serverInfo") or {}).get("name"), "pya-lsp", "serverInfo.name")


def test_initialized_and_unknown(c):
    c.notify("initialized", {})
    c.send({"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting", "params": {}})
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



TEXT_FEATURES = (
    "import math\n"
    "\n"
    "\n"
    "def helper(n: int) -> int:\n"
    "    return n\n"
    "\n"
    "\n"
    "class Widget:\n"
    "    pass\n"
    "\n"
    "\n"
    "VALUE: int = 0\n"
    "\n"
    "\n"
    "def use() -> int:\n"
    "    return helper(VALUE)\n"
    "    sq\n"
)

TEXT_TOKENS = (
    "# 注释：中文 😀\n"
    "class Point:\n"
    "    pass\n"
    "\n"
    "name: str = \"中文 😀 ünïcode\"\n"
    "count: int = 42\n"
    "\n"
    "def greet(who: str) -> str:\n"
    "    return \"hi \" + who + str(Point)\n"
    "def documented() -> None:\n"
    '    """第一行\n'
    "    中文 😀 第二行\n"
    '    """\n'
    "    pass\n"
    "\n"
    "hexval: int = 0xFF\n"
)

def utf16_len(s):
    total = 0
    for ch in s:
        total += 2 if ord(ch) > 0xFFFF else 1
    return total

def decode_semantic_tokens(data):
    # 标准 5 元组 -> [(line, start, length, type), ...]
    out = []
    line = 0
    start = 0
    i = 0
    while i + 4 < len(data):
        dl = data[i]
        ds = data[i + 1]
        ln = data[i + 2]
        tt = data[i + 3]
        if dl == 0:
            start = start + ds
        else:
            line = line + dl
            start = ds
        out.append((line, start, ln, tt))
        i += 5
    return out

def completion_labels(res):
    result = res.get("result")
    if isinstance(result, dict):
        items = result.get("items") or []
    else:
        items = result or []
    labels = []
    for item in items:
        labels.append(item.get("label"))
    return labels, items

def test_completion(c, uri):
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "python", "version": 1, "text": TEXT_FEATURES}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    # 前缀过滤：光标在 "helper" 中间 -> 只剩以 helper 开头的候选
    t0 = time.monotonic()
    res = c.request(300, "textDocument/completion", {
        "textDocument": {"uri": uri}, "position": {"line": 15, "character": 17}})
    elapsed = time.monotonic() - t0
    print("    completion round-trip: %.3fs" % elapsed)
    expect(elapsed < 1.0, "completion round-trip < 1s (got %.3fs)" % elapsed)
    labels, items = completion_labels(res)
    expect("helper" in labels, "completion includes document function (got %r)" % labels[:10])
    expect(len(labels) > 0 and all(l.startswith("helper") for l in labels),
           "completion prefix filter (got %r)" % labels[:10])
    # import 成员 + 关键字：空前缀时三类都在
    res2 = c.request(301, "textDocument/completion", {
        "textDocument": {"uri": uri}, "position": {"line": 15, "character": 4}})
    labels2, _ = completion_labels(res2)
    expect("helper" in labels2, "completion empty prefix includes function")
    expect("Widget" in labels2, "completion empty prefix includes class")
    expect("VALUE" in labels2, "completion empty prefix includes variable")
    expect("math" in labels2, "completion empty prefix includes imported module")
    expect("sqrt" in labels2, "completion empty prefix includes module member")
    expect("def" in labels2, "completion empty prefix includes keyword")
    expect(len(labels2) <= 100, "completion limited to <=100 items (got %d)" % len(labels2))
    # 前缀 "sq" -> sqrt，且不含 helper
    res3 = c.request(302, "textDocument/completion", {
        "textDocument": {"uri": uri}, "position": {"line": 16, "character": 6}})
    labels3, _ = completion_labels(res3)
    expect("sqrt" in labels3, "completion prefix 'sq' finds sqrt (got %r)" % labels3)
    expect("helper" not in labels3, "completion prefix filters out non-matching")
    # 候选带 kind/detail 字段
    if items:
        first = items[0]
        expect("kind" in first, "completion item has kind")
        expect("detail" in first, "completion item has detail")

def def_location(res):
    result = res.get("result")
    if not isinstance(result, dict):
        return None
    rng = result.get("range") or {}
    start = rng.get("start") or {}
    return (result.get("uri"), start.get("line"), start.get("character"))

def test_definition(c, uri):
    # helper 调用点 -> def helper 行（0-based 3，character 4）
    t0 = time.monotonic()
    res = c.request(310, "textDocument/definition", {
        "textDocument": {"uri": uri}, "position": {"line": 15, "character": 13}})
    elapsed = time.monotonic() - t0
    print("    definition round-trip: %.3fs" % elapsed)
    expect(elapsed < 1.0, "definition round-trip < 1s (got %.3fs)" % elapsed)
    loc = def_location(res)
    expect(loc is not None, "definition on helper returns a Location")
    if loc is not None:
        expect_eq(loc[0], uri, "definition location uri")
        expect_eq(loc[1], 3, "definition jumps to def helper line")
        expect_eq(loc[2], 4, "definition column is name start")
    # 形参 n -> 所在函数定义
    res2 = c.request(311, "textDocument/definition", {
        "textDocument": {"uri": uri}, "position": {"line": 4, "character": 11}})
    loc2 = def_location(res2)
    expect(loc2 is not None and loc2[1] == 3,
           "definition on parameter jumps to enclosing def (got %r)" % (loc2,))
    # 顶层赋值 VALUE 自身 -> 定义行
    res3 = c.request(312, "textDocument/definition", {
        "textDocument": {"uri": uri}, "position": {"line": 15, "character": 20}})
    loc3 = def_location(res3)
    expect(loc3 is not None and loc3[1] == 11, "definition on assignment (got %r)" % (loc3,))
    # 找不到 -> null（合法的 LSP 结果）
    res4 = c.request(313, "textDocument/definition", {
        "textDocument": {"uri": uri}, "position": {"line": 0, "character": 2}})
    expect_eq(res4.get("result"), None, "definition on unknown word is null")

def test_semantic_tokens(c, uri):
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "python", "version": 1, "text": TEXT_TOKENS}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    t0 = time.monotonic()
    res = c.request(320, "textDocument/semanticTokens/full", {
        "textDocument": {"uri": uri}})
    elapsed = time.monotonic() - t0
    print("    semanticTokens round-trip: %.3fs" % elapsed)
    expect(elapsed < 1.0, "semanticTokens round-trip < 1s (got %.3fs)" % elapsed)
    result = res.get("result") or {}
    data = result.get("data") or []
    expect(len(data) > 0, "semanticTokens data non-empty")
    expect(len(data) % 5 == 0, "semanticTokens 5-tuple encoding")
    tokens = decode_semantic_tokens(data)
    lines = TEXT_TOKENS.split("\n")

    # 非 ASCII 关键断言：注释与字符串 token 的 UTF-16 长度（😀 占 2 个 unit）
    expect((0, 0, utf16_len(lines[0]), 6) in tokens,
           "comment token covers full unicode comment line (want len %d)" % utf16_len(lines[0]))
    want_str_start = lines[4].index("\"")
    want_str_len = utf16_len(lines[4][want_str_start:])
    expect((4, want_str_start, want_str_len, 4) in tokens,
           "string token UTF-16 length on non-ASCII line (want start %d len %d)"
           % (want_str_start, want_str_len))
    num_start = lines[5].index("42")
    expect((5, num_start, 2, 5) in tokens, "number token")
    expect((1, 0, 5, 3) in tokens, "class keyword token")
    expect((1, len("class "), len("Point"), 0) in tokens, "class name token")
    expect((7, 0, 3, 3) in tokens, "def keyword token")
    expect((7, 4, len("greet"), 1) in tokens, "function name token")
    expect((8, 4, 6, 3) in tokens, "return keyword token")
    # 三引号跨行字符串：docstring 每行都是 string token
    expect((9, 0, 3, 3) in tokens, "def keyword before docstring")
    expect((9, 4, len("documented"), 1) in tokens, "documented function name token")
    expect((10, 4, utf16_len(lines[10]) - 4, 4) in tokens,
           "docstring first line string token (unicode)")
    expect((11, 0, utf16_len(lines[11]), 4) in tokens,
           "docstring middle line whole-line string token (unicode)")
    expect((12, 4, 3, 4) in tokens, "docstring closing quotes token")
    hex_start = lines[15].index("0xFF")
    expect((15, hex_start, 4, 5) in tokens, "hex number token")
    # 所有 token 都在 UTF-16 行范围内
    ok_bounds = True
    for (ln, st, length, tt) in tokens:
        if ln < 0 or ln >= len(lines) or st < 0 or st + length > utf16_len(lines[ln]):
            ok_bounds = False
    expect(ok_bounds, "all semantic tokens fit inside their line (UTF-16)")

def test_debounce(c, uri):
    # 连发两次 didChange（中间不读回包）：服务器应合并，只发布最终版本的诊断。
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "python", "version": 1, "text": TEXT_BAD}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    t0 = time.monotonic()
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 2},
        "contentChanges": [{"text": TEXT_FIXED}]})
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 3},
        "contentChanges": [{"text": TEXT_BAD}]})
    params = diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    elapsed = time.monotonic() - t0
    print("    debounced publish after %.3fs" % elapsed)
    expect_eq(params.get("version"), 3, "debounce publishes only final version")
    expect(len(params.get("diagnostics") or []) >= 1,
           "debounce final diagnostics reflect last change")
    expect(elapsed < 1.5, "debounce window does not stall (<1.5s, got %.3fs)" % elapsed)
    # 确认没有为 version 2 单独发布过（否则客户端会看到中间态闪一下）
    stale = [m for m in c.pending
             if m.get("method") == "textDocument/publishDiagnostics"
             and (m.get("params") or {}).get("version") == 2]
    expect(len(stale) == 0, "no intermediate diagnostics for version 2")
    # 一次 didChange 仍按最终内容发布（v4 改回合法 -> 诊断清空）
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 4},
        "contentChanges": [{"text": TEXT_FIXED}]})
    params2 = diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    expect_eq(params2.get("version"), 4, "single didChange still publishes")
    expect_eq(params2.get("diagnostics"), [], "single didChange clears diagnostics")
    # debounce 窗口内到达的请求不能被窗口拖住：先发 didChange 再立刻发 hover，
    # hover 必须在 <1s 内回包（诊断通知会被 Client 缓冲到 pending）。
    c.notify("textDocument/didChange", {
        "textDocument": {"uri": uri, "version": 5},
        "contentChanges": [{"text": TEXT_FIXED + "\n"}]})
    t1 = time.monotonic()
    res_hover = c.request(770, "textDocument/hover", {
        "textDocument": {"uri": uri}, "position": {"line": 0, "character": 5}})
    hover_elapsed = time.monotonic() - t1
    print("    request during debounce window answered in %.3fs" % hover_elapsed)
    expect(res_hover.get("id") == 770, "request during debounce answered")
    expect(hover_elapsed < 1.0,
           "request not stalled by debounce window (got %.3fs)" % hover_elapsed)
    # 队列里的请求处理完后，debounce 的诊断通知才到（版本 5）；消费掉它，
    # 免得污染后续文档的通知断言。
    diag5 = diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    expect_eq(diag5.get("version"), 5, "debounce publishes after queued request")
    c.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri)

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



# ---------------- references / rename（追加：文档内符号查找与重命名） ----------------

TEXT_REFS = (
    "def clamp(x: int, lo: int) -> int:\n"
    "    if x < lo:\n"
    "        return lo\n"
    "    if x > lo:\n"
    "        return lo + x\n"
    "    return x\n"
    "\n"
    "limit: int = 10\n"
    "\n"
    "def other() -> int:\n"
    "    x: int = 3\n"
    "    if x > limit:\n"
    "        return clamp(x, x)\n"
    "    return x\n"
    "\n"
    "print(\"\U0001F600\", limit)\n"
    "\n"
    "# 注释里也有 x、clamp 和 limit，不应算引用\n"
    "note: str = \"字符串里的 x 也不算\"\n"
)


def nth_index(text, line, word, nth):
    raw = text.split("\n")[line]
    start = 0
    idx = -1
    for _ in range(nth + 1):
        idx = raw.find(word, start)
        start = idx + 1
    return idx


def refs_pos(text, line, word, nth=0):
    return {"line": line, "character": nth_index(text, line, word, nth)}


def utf16_to_cp_index(s, units):
    total = 0
    i = 0
    while i < len(s) and total < units:
        total += 2 if ord(s[i]) > 0xFFFF else 1
        i += 1
    return i


def loc_tuples(result):
    out = []
    for loc in (result or []):
        rng = loc.get("range") or {}
        start = rng.get("start") or {}
        end = rng.get("end") or {}
        out.append((start.get("line"), start.get("character"),
                    end.get("character")))
    return out


def test_references_and_rename(c, uri):
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "python", "version": 1, "text": TEXT_REFS}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri)

    # ---- references：函数形参 x 只匹配 clamp 函数体内的 4 处使用 ----
    t0 = time.monotonic()
    res = c.request(400, "textDocument/references", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 1, "x"),
        "context": {"includeDeclaration": False}})
    elapsed = time.monotonic() - t0
    print("    references round-trip: %.3fs" % elapsed)
    expect(elapsed < 1.0, "references round-trip < 1s (got %.3fs)" % elapsed)
    locs = loc_tuples(res.get("result"))
    expect_eq(len(locs), 4, "references: 4 uses of parameter x (got %r)" % (locs,))
    expect_eq([l[0] for l in locs], [1, 3, 4, 5], "references lines for parameter x")
    expect(all(l[1] >= 0 and l[2] == l[1] + 1 for l in locs),
           "references ranges cover exactly 'x'")
    # 同名局部变量在 other 里，不应混进来；注释/字符串里的 x 也不算
    expect(not any(l[0] >= 15 for l in locs), "references exclude comment/string lines")

    # includeDeclaration=True：定义（形参）也计入
    res = c.request(401, "textDocument/references", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 1, "x"),
        "context": {"includeDeclaration": True}})
    locs2 = loc_tuples(res.get("result"))
    expect_eq(len(locs2), 5, "references with declaration includes param (got %r)" % (locs2,))
    expect((0, 10, 11) in locs2, "references declaration position is the parameter")

    # ---- references：另一个函数里的同名局部变量只匹配自己的作用域 ----
    res = c.request(402, "textDocument/references", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 10, "x"),
        "context": {"includeDeclaration": False}})
    locs3 = loc_tuples(res.get("result"))
    expect_eq(len(locs3), 4, "references: local x in other has 4 uses (got %r)" % (locs3,))
    expect_eq([l[0] for l in locs3], [11, 12, 12, 13],
              "references local x stays inside other()")

    # ---- references：全局变量 limit（含 UTF-16 列：😀 占 2 units） ----
    res = c.request(403, "textDocument/references", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 7, "limit"),
        "context": {"includeDeclaration": False}})
    locs4 = loc_tuples(res.get("result"))
    expect_eq(len(locs4), 2, "references: limit has 2 uses (got %r)" % (locs4,))
    expect_eq(locs4, [(11, 11, 16), (15, 12, 17)],
              "references: limit positions incl UTF-16 column after emoji")

    # ---- references：函数名 clamp ----
    res = c.request(404, "textDocument/references", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 0, "clamp"),
        "context": {"includeDeclaration": False}})
    expect_eq(loc_tuples(res.get("result")), [(12, 15, 20)],
              "references: clamp call site")

    # ---- references：无法解析的普通词 -> [] ----
    res = c.request(405, "textDocument/references", {
        "textDocument": {"uri": uri},
        "position": {"line": 15, "character": 3},
        "context": {"includeDeclaration": True}})
    expect_eq(res.get("result"), [], "references on unknown word is empty")

    # ---- rename：改函数名 -> WorkspaceEdit 覆盖定义+调用，range 文本是原名 ----
    t0 = time.monotonic()
    res = c.request(410, "textDocument/rename", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 0, "clamp"),
        "newName": "bound"})
    elapsed = time.monotonic() - t0
    print("    rename round-trip: %.3fs" % elapsed)
    expect(elapsed < 1.0, "rename round-trip < 1s (got %.3fs)" % elapsed)
    result = res.get("result") or {}
    changes = result.get("changes") or {}
    edits = changes.get(uri) or []
    expect(uri in changes, "rename returns changes keyed by uri")
    expect_eq(len(edits), 2, "rename function: definition + call (got %d)" % len(edits))
    lines = TEXT_REFS.split("\n")
    ok_text = True
    ok_new = True
    for e in edits:
        rng = e.get("range") or {}
        st = rng.get("start") or {}
        en = rng.get("end") or {}
        line_txt = lines[st.get("line")]
        a = utf16_to_cp_index(line_txt, st.get("character"))
        b = utf16_to_cp_index(line_txt, en.get("character"))
        if line_txt[a:b] != "clamp":
            ok_text = False
        if e.get("newText") != "bound":
            ok_new = False
    expect(ok_text, "rename ranges cover exactly the old name")
    expect(ok_new, "rename newText is newName")
    expect_eq(sorted(e.get("range", {}).get("start", {}).get("line") for e in edits),
              [0, 12], "rename function edits lines")

    # ---- rename：局部变量只改自己作用域 ----
    res = c.request(411, "textDocument/rename", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 10, "x"),
        "newName": "tmp"})
    edits2 = ((res.get("result") or {}).get("changes") or {}).get(uri) or []
    expect_eq(len(edits2), 5, "rename local x: decl + 4 uses (got %d)" % len(edits2))
    expect_eq(sorted(e.get("range", {}).get("start", {}).get("line") for e in edits2),
              [10, 11, 12, 12, 13], "rename local x stays inside other()")

    # ---- rename：全局变量 limit（含 😀 同行的 UTF-16 列） ----
    res = c.request(412, "textDocument/rename", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 7, "limit"),
        "newName": "maxv"})
    edits3 = ((res.get("result") or {}).get("changes") or {}).get(uri) or []
    expect_eq(len(edits3), 3, "rename limit: decl + 2 uses (got %d)" % len(edits3))
    chars = sorted(e.get("range", {}).get("start", {}).get("character") for e in edits3)
    expect_eq(chars, [0, 11, 12], "rename limit UTF-16 starts")

    # ---- rename：非法 newName -> InvalidParams(-32602) ----
    res = c.request(413, "textDocument/rename", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 0, "clamp"),
        "newName": "a-b"})
    err = res.get("error") or {}
    expect_eq(err.get("code"), -32602, "rename invalid identifier -> InvalidParams")

    # ---- rename：关键字 / 内置类型 / 未声明词 -> null ----
    res = c.request(414, "textDocument/rename", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 1, "return"),
        "newName": "ret"})
    expect_eq(res.get("result"), None, "rename keyword -> null")
    res = c.request(415, "textDocument/rename", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 18, "str"),
        "newName": "text"})
    expect_eq(res.get("result"), None, "rename builtin type -> null")
    res = c.request(416, "textDocument/rename", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 15, "print"),
        "newName": "emit"})
    expect_eq(res.get("result"), None, "rename builtin function -> null")
    # 关键字不是符号：references 返回 []
    res = c.request(406, "textDocument/references", {
        "textDocument": {"uri": uri},
        "position": refs_pos(TEXT_REFS, 1, "return"),
        "context": {"includeDeclaration": True}})
    expect_eq(res.get("result"), [], "references on keyword is empty")

    c.notify("textDocument/didClose", {"textDocument": {"uri": uri}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri)
# ---------------- 新增：signatureHelp / documentHighlight / foldingRange ----------------

TEXT_SIGHELP = (
    "def add(a: int, b: int) -> int:\n"
    "    return a + b\n"
    "\n"
    "\n"
    "def noargs() -> int:\n"
    "    return 1\n"
    "\n"
    "\n"
    "def call_noargs() -> int:\n"
    "    return noargs()\n"
    "\n"
    "\n"
    "def use() -> int:\n"
    "    return add(1, 2)\n"
    "\n"
    "\n"
    "def use_str() -> int:\n"
    '    return add("x,y", 2)\n'
    "\n"
    "\n"
    "def uni() -> int:\n"
    '    return add("中文😀", 2)\n'
    "\n"
    "\n"
    "class Widget:\n"
    "    def norm(self, k: int) -> int:\n"
    "        return k\n"
    "\n"
    "\n"
    "def call_method(w: Widget) -> int:\n"
    "    return w.norm(1, 2)\n"
    "\n"
    "\n"
    "def multi() -> int:\n"
    "    return add(\n"
    "        1,\n"
    "        2)\n"
)


def find_pos(text, needle, offset=0):
    # needle 首次出现处 -> (行号, 该行内 UTF-16 列)；offset 是 needle 内的码点偏移。
    lines = text.split("\n")
    for i, ln in enumerate(lines):
        j = ln.find(needle)
        if j >= 0:
            return (i, utf16_len(ln[:j + offset]))
    raise AssertionError("needle not found: %r" % needle)


def signature_help_result(res):
    result = res.get("result")
    if not isinstance(result, dict):
        return None
    return result


def signature_active(res):
    sh = signature_help_result(res)
    if sh is None:
        return None
    return sh.get("activeParameter")


def test_signature_help(c, uri):
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "python", "version": 1, "text": TEXT_SIGHELP}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    # f(1, |2) 光标在第二参 -> activeParameter=1
    line, ch = find_pos(TEXT_SIGHELP, "add(1, 2)", len("add(1, "))
    t0 = time.monotonic()
    res = c.request(400, "textDocument/signatureHelp", {
        "textDocument": {"uri": uri}, "position": {"line": line, "character": ch}})
    elapsed = time.monotonic() - t0
    print("    signatureHelp round-trip: %.3fs" % elapsed)
    expect(elapsed < 1.0, "signatureHelp round-trip < 1s (got %.3fs)" % elapsed)
    sh = signature_help_result(res)
    expect(sh is not None, "signatureHelp in call parens returns SignatureHelp")
    if sh is not None:
        sigs = sh.get("signatures") or []
        expect_eq(len(sigs), 1, "signatureHelp has one signature")
        if sigs:
            sig = sigs[0]
            label = sig.get("label") or ""
            expect("add" in label, "signature label names the callee (got %r)" % label)
            expect_in("a: int", label, "signature label contains parameter text")
            ps = sig.get("parameters") or []
            expect_eq(len(ps), 2, "signatureHelp parameters count")
            if len(ps) >= 2:
                expect_eq(ps[1].get("label"), "b: int", "second parameter label")
            expect_eq(sig.get("activeParameter"), 1, "signature activeParameter = 1")
        expect_eq(sh.get("activeParameter"), 1, "top-level activeParameter = 1")
    # 光标在第一参 -> 0
    line0, ch0 = find_pos(TEXT_SIGHELP, "add(1, 2)", len("add("))
    res0 = c.request(401, "textDocument/signatureHelp", {
        "textDocument": {"uri": uri}, "position": {"line": line0, "character": ch0}})
    expect_eq(signature_active(res0), 0, "signatureHelp first parameter activeParameter=0")
    # 无参函数 -> parameters 为空
    ln1, ch1 = find_pos(TEXT_SIGHELP, "return noargs()", len("return noargs("))
    res1 = c.request(402, "textDocument/signatureHelp", {
        "textDocument": {"uri": uri}, "position": {"line": ln1, "character": ch1}})
    sh1 = signature_help_result(res1)
    expect(sh1 is not None, "signatureHelp for no-arg call returns SignatureHelp")
    if sh1 is not None:
        sigs1 = sh1.get("signatures") or []
        expect(len(sigs1) == 1 and (sigs1[0].get("parameters") or []) == [],
               "no-arg function has empty parameters[]")
    # 字符串里的逗号不参与 activeParameter
    ln2, ch2 = find_pos(TEXT_SIGHELP, 'add("x,y", 2)', len('add("x,y", '))
    res2 = c.request(403, "textDocument/signatureHelp", {
        "textDocument": {"uri": uri}, "position": {"line": ln2, "character": ch2}})
    expect_eq(signature_active(res2), 1, "comma inside string is not counted")
    # 非 ASCII 同行（emoji 在光标前，UTF-16 列换算）：add("中文😀", |2)
    ln3, ch3 = find_pos(TEXT_SIGHELP, 'add("中文😀", 2)', len('add("中文😀", '))
    res3 = c.request(404, "textDocument/signatureHelp", {
        "textDocument": {"uri": uri}, "position": {"line": ln3, "character": ch3}})
    expect_eq(signature_active(res3), 1,
              "signatureHelp handles non-ASCII before cursor (UTF-16 column)")
    # 跨行调用：光标落在第二个实参行 -> 仍能识别 add 的括号并数逗号
    ln5, ch5 = find_pos(TEXT_SIGHELP, "        2)", len("        "))
    res5 = c.request(406, "textDocument/signatureHelp", {
        "textDocument": {"uri": uri}, "position": {"line": ln5, "character": ch5}})
    expect_eq(signature_active(res5), 1, "signatureHelp works on multi-line call")
    # 方法调用：w.norm(1, |2) -> self,k 里 k 是第二参
    ln6, ch6 = find_pos(TEXT_SIGHELP, "w.norm(1, 2)", len("w.norm(1, "))
    res6 = c.request(407, "textDocument/signatureHelp", {
        "textDocument": {"uri": uri}, "position": {"line": ln6, "character": ch6}})
    sh6 = signature_help_result(res6)
    expect(sh6 is not None and "norm" in ((sh6.get("signatures") or [{}])[0].get("label") or ""),
           "signatureHelp resolves method-call callee")
    expect_eq(signature_active(res6), 1, "method call activeParameter counts commas")
    # 光标不在调用括号内 -> null
    ln4, ch4 = find_pos(TEXT_SIGHELP, "return a + b", len("return a"))
    res4 = c.request(405, "textDocument/signatureHelp", {
        "textDocument": {"uri": uri}, "position": {"line": ln4, "character": ch4}})
    expect_eq(res4.get("result"), None, "signatureHelp outside call parens is null")


TEXT_HIGHLIGHT = (
    "x: int = 1\n"
    "\n"
    "def show() -> int:\n"
    "    y: int = x + 1\n"
    "    return x + y\n"
)

TEXT_HIGHLIGHT_LEX = (
    "x: int = 1\n"
    "# x in comment\n"
    's: str = "x in string"\n'
    "print(x)\n"
)

TEXT_HIGHLIGHT_UNI = (
    "# 中文 😀\n"
    "x: int = 1\n"
    'label: str = "中文😀" + str(x)\n'
)


def highlight_list(res):
    result = res.get("result")
    if not isinstance(result, list):
        return None
    return result


def highlight_starts(res):
    ranges = highlight_list(res) or []
    out = []
    for r in ranges:
        start = (r.get("range") or {}).get("start") or {}
        out.append(start.get("line", -1))
    return out


def test_document_highlight(c, uri):
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "python", "version": 1, "text": TEXT_HIGHLIGHT}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    t0 = time.monotonic()
    res = c.request(410, "textDocument/documentHighlight", {
        "textDocument": {"uri": uri}, "position": {"line": 0, "character": 0}})
    elapsed = time.monotonic() - t0
    print("    documentHighlight round-trip: %.3fs" % elapsed)
    expect(elapsed < 1.0, "documentHighlight round-trip < 1s (got %.3fs)" % elapsed)
    ranges = highlight_list(res)
    expect(ranges is not None and len(ranges) == 3,
           "highlight finds all 3 occurrences of x (got %r)" % (ranges,))
    if ranges and len(ranges) == 3:
        starts = [(r.get("range") or {}).get("start") or {} for r in ranges]
        expect_eq([s.get("line") for s in starts], [0, 3, 4], "highlight occurrence lines")
        lines = TEXT_HIGHLIGHT.split("\n")
        expect_eq(starts[1].get("character"), lines[3].index("x"),
                  "highlight second occurrence column")
        expect_eq(starts[2].get("character"), lines[4].index("x"),
                  "highlight third occurrence column")
        expect_eq(ranges[0].get("kind"), 3, "assignment target highlighted as Write(3)")
        expect_eq(ranges[1].get("kind"), 1, "plain read highlighted as Text(1)")
    # 注释 / 字符串里的同名 token 不算出现
    uri2 = uri + ".lex"
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri2, "languageId": "python", "version": 1, "text": TEXT_HIGHLIGHT_LEX}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri2)
    res2 = c.request(411, "textDocument/documentHighlight", {
        "textDocument": {"uri": uri2}, "position": {"line": 0, "character": 0}})
    expect_eq(highlight_starts(res2), [0, 3],
              "highlight ignores occurrences inside comment/string")
    # 非 ASCII 行：行号不受影响，列按 UTF-16 code unit
    uri3 = uri + ".uni"
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri3, "languageId": "python", "version": 1, "text": TEXT_HIGHLIGHT_UNI}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri3)
    res3 = c.request(412, "textDocument/documentHighlight", {
        "textDocument": {"uri": uri3}, "position": {"line": 1, "character": 0}})
    starts3 = highlight_starts(res3)
    expect_eq(starts3, [1, 2], "highlight lines on non-ASCII document")
    ranges3 = highlight_list(res3) or []
    if len(ranges3) >= 2:
        line2 = TEXT_HIGHLIGHT_UNI.split("\n")[2]
        cp_start = line2.index("x")
        want_u16 = utf16_len(line2[:cp_start])
        got_u16 = ((ranges3[1].get("range") or {}).get("start") or {}).get("character")
        expect_eq(got_u16, want_u16, "highlight UTF-16 column on non-ASCII line")
        expect(want_u16 > cp_start,
               "non-ASCII line really shifts UTF-16 column (emoji before x)")


TEXT_FOLD = (
    "# 第一行注释\n"
    "# 第二行注释\n"
    "\n"
    "def foo(n: int) -> int:\n"
    "    a: int = n\n"
    "    b: int = a + 1\n"
    "    return b\n"
    "\n"
    "class Bar:\n"
    "    x: int\n"
    "    y: int\n"
    "\n"
    "def q() -> None:\n"
    '    """doc line1\n'
    "    doc line2 中文😀\n"
    '    """\n'
    "    pass\n"
    "\n"
    "def one() -> int:\n"
    "    return 1\n"
)


def folding_tuples(res):
    result = res.get("result")
    if not isinstance(result, list):
        return None
    out = []
    for r in result:
        out.append((r.get("startLine"), r.get("endLine"), r.get("kind")))
    return out


def test_folding_range(c, uri):
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri, "languageId": "python", "version": 1, "text": TEXT_FOLD}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri)
    t0 = time.monotonic()
    res = c.request(420, "textDocument/foldingRange", {"textDocument": {"uri": uri}})
    elapsed = time.monotonic() - t0
    print("    foldingRange round-trip: %.3fs" % elapsed)
    expect(elapsed < 1.0, "foldingRange round-trip < 1s (got %.3fs)" % elapsed)
    folds = folding_tuples(res)
    expect(folds is not None and len(folds) >= 4,
           "foldingRange returns regions (got %r)" % (folds,))
    folds = folds or []
    expect((0, 1, "comment") in folds, "consecutive comment block folds")
    # 验收点：3 行函数体 -> 覆盖 def 行(3) 到最后一行(6) 的 1 个 region
    expect((3, 6, "region") in folds, "3-line function body folds def..last line")
    expect_eq(len([f for f in folds if f[0] == 3]), 1, "exactly one fold starts at def line")
    expect((8, 10, "region") in folds, "class block folds")
    expect((12, 16, "region") in folds, "function with docstring folds")
    expect((13, 15, "region") in folds, "triple-quoted string block folds")
    expect((18, 19, "region") in folds, "minimal 2-line function folds")
    ok_order = True
    ok_bounds = True
    prev = -1
    for (sl, el, kind) in folds:
        if not isinstance(sl, int) or not isinstance(el, int) or el <= sl:
            ok_bounds = False
        if sl < prev:
            ok_order = False
        prev = sl
    expect(ok_bounds, "every folding range has startLine < endLine")
    expect(ok_order, "folding ranges sorted by startLine")
    # 空文档 -> []
    uri2 = uri + ".empty"
    c.notify("textDocument/didOpen", {"textDocument": {
        "uri": uri2, "languageId": "python", "version": 1, "text": ""}})
    diag_params(c.notification("textDocument/publishDiagnostics"), uri2)
    res2 = c.request(421, "textDocument/foldingRange", {"textDocument": {"uri": uri2}})
    expect_eq(res2.get("result"), [], "foldingRange on empty document is []")


def test_new_capabilities(server, compiler):
    # 新方法必须在 initialize 的 capabilities 里声明（独立起一个客户端，不动主会话）。
    c = make_client(server, compiler)
    try:
        res = c.request(430, "initialize", {
            "processId": None, "rootUri": None, "capabilities": {}})
        caps = (res.get("result") or {}).get("capabilities") or {}
        sh = caps.get("signatureHelpProvider")
        expect(isinstance(sh, dict) and "(" in (sh.get("triggerCharacters") or []),
               "capabilities advertise signatureHelpProvider with trigger chars")
        expect_eq(caps.get("documentHighlightProvider"), True,
                  "capabilities advertise documentHighlightProvider")
        expect_eq(caps.get("foldingRangeProvider"), True,
                  "capabilities advertise foldingRangeProvider")
        res2 = c.request(431, "shutdown", None)
        expect_eq(res2.get("result"), None, "capability probe shutdown ok")
        c.notify("exit", None)
        rc = c.wait_exit()
        expect_eq(rc, 0, "capability probe client exits cleanly")
    finally:
        if c.proc.poll() is None:
            c.proc.kill()


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
    uri_d = "file:///tmp/pya-lsp-smoke-features.py"
    uri_e = "file:///tmp/pya-lsp-smoke-tokens.py"
    uri_f = "file:///tmp/pya-lsp-smoke-debounce.py"
    uri_g = "file:///tmp/pya-lsp-smoke-refs.py"
    uri_s = "file:///tmp/pya-lsp-smoke-sighelp.py"
    uri_hl = "file:///tmp/pya-lsp-smoke-highlight.py"
    uri_fold = "file:///tmp/pya-lsp-smoke-fold.py"

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
        print("[7c] completion (document symbols / import members / keywords)")
        test_completion(c, uri_d)
        print("[7d] definition (def/class/assignment, null when unknown)")
        test_definition(c, uri_d)
        print("[7e] semanticTokens/full (non-ASCII UTF-16 encoding)")
        test_semantic_tokens(c, uri_e)
        print("[7f] didChange debounce")
        test_debounce(c, uri_f)
        print("[7g] signatureHelp (activeParameter / no-arg / strings / UTF-16)")
        test_signature_help(c, uri_s)
        print("[7h] documentHighlight (same-name / comment+string / UTF-16)")
        test_document_highlight(c, uri_hl)
        print("[7i] foldingRange (def/class/comment/triple-string)")
        test_folding_range(c, uri_fold)
        print("[7j] new capabilities advertised in initialize")
        test_new_capabilities(server, compiler)
        print("[8] unicode framing")
        test_unicode(c, uri_c)
        print("[9] malformed JSON resilience")
        test_malformed_json(c, uri_b)
        print("[7g] references / rename")
        test_references_and_rename(c, uri_g)
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
