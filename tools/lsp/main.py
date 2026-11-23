# tools/lsp/main.py
# PyAdamas 写的 Language Server（stdio 传输 + 全量文档同步 + 诊断 + documentSymbol + hover）。
#
# 构建：tools/lsp/build.sh  →  pya-lsp
# 运行：./pya-lsp [--stdio] [<pyadamas 编译器路径>]
#   - 诊断通过 subprocess 调 "<编译器> check <tmpfile>"；编译器路径优先级：
#     $PYADAMAS > argv[1]（不以 '-' 开头时）> PATH 里的 pyadamas
#   - $PYA_LSP_DEBUG 非空时把调试日志打到 stderr
#   - 协议帧：Content-Length 头 + 空行 + 恰好 N 字节 JSON；回包同样的帧并 flush

import sys
import os
import time
import subprocess
import minijson

# ---------------- 环境 / 日志 ----------------

def env_str(name: str) -> str:
    v: str = os.getenv(name)
    if v == "":
        return ""
    return v

def flag_on(name: str) -> bool:
    return env_str(name) != ""

def log(msg: str) -> None:
    if flag_on("PYA_LSP_DEBUG"):
        sys.stderr.write("[pya-lsp] " + msg + "\n")
        sys.stderr.flush()

def compiler_path() -> str:
    v: str = env_str("PYADAMAS")
    if v != "":
        return v
    if len(sys.argv) > 1:
        c: str = sys.argv[1]
        if not c.startswith("-"):
            return c
    return "pyadamas"

tmp_seq: int = 0

def temp_file_path() -> str:
    global tmp_seq
    tmp_seq = tmp_seq + 1
    base: str = env_str("TMPDIR")
    if base == "":
        base = "/tmp"
    if base.endswith("/"):
        base = base[:len(base) - 1]
    stamp: int = int(time.time() * 1000000.0)
    return base + "/pya-lsp-" + str(stamp) + "-" + str(tmp_seq) + ".py"

def write_file(path: str, text: str) -> bool:
    try:
        f = open(path, "w")
        f.write(text)
        f.close()
        return True
    except OSError:
        return False

def remove_file(path: str) -> None:
    try:
        os.remove(path)
    except OSError:
        pass

# ---------------- 小工具 ----------------

def is_digit_str(s: str) -> bool:
    if s == "":
        return False
    i: int = 0
    while i < len(s):
        o: int = ord(s[i])
        if o < 48 or o > 57:
            return False
        i = i + 1
    return True

def first_non_space(s: str, start: int) -> int:
    i: int = start
    while i < len(s):
        c: str = s[i]
        if c == " " or c == "\t":
            i = i + 1
        else:
            return i
    return i

def is_ident_char(c: str) -> bool:
    o: int = ord(c)
    if o >= 65 and o <= 90:
        return True
    if o >= 97 and o <= 122:
        return True
    if o >= 48 and o <= 57:
        return True
    return o == 95

def read_ident(s: str, start: int) -> str:
    i: int = first_non_space(s, start)
    begin: int = i
    while i < len(s) and is_ident_char(s[i]):
        i = i + 1
    return s[begin:i]

def indent_width(line: str) -> int:
    width: int = 0
    i: int = 0
    while i < len(line):
        c: str = line[i]
        if c == " ":
            width = width + 1
        elif c == "\t":
            width = width + 8
        else:
            return width
        i = i + 1
    return width

def truncate(s: str, n: int) -> str:
    if len(s) <= n:
        return s
    return s[:n] + " ..."

def strip_trailing_colon(s: str) -> str:
    if len(s) > 0 and s[len(s) - 1] == ":":
        return s[:len(s) - 1]
    return s

# ---------------- JSON-RPC 帧 ----------------

def open_stdin() -> file:
    # 关键：必须用「二进制模式」的独立 FILE* 读 stdin。
    # sys.stdin 是文本模式，其 file_fill 用 fread(..., 8192, ...)：glibc 的 fread
    # 会阻塞到填满 8192 字节或 EOF，管道上就死锁了（LSP 客户端写完一帧就等回包）。
    # 二进制分支的 pa_file_read 是 fread(chunk, 1, n)，n 字节到齐就返回；
    # readline 走 fgetc，也只等到行尾。实测可交互。
    try:
        return open("/dev/stdin", "rb")
    except OSError:
        log("cannot open /dev/stdin in binary mode, falling back to sys.stdin")
        return sys.stdin

def read_frame(inp: file) -> str:
    length: int = -1
    while True:
        line: str = inp.readline()
        if line == "":
            return ""
        stripped: str = line.strip()
        if stripped == "":
            break
        if stripped.startswith("Content-Length:"):
            num: str = stripped[len("Content-Length:"):].strip()
            if is_digit_str(num):
                length = int(num)
                log("content-length=" + num)
    if length <= 0:
        return ""
    body: str = inp.read(length)
    if body == "":
        return ""
    return body

def send_frame(body: str) -> None:
    # 所有响应体都是纯 ASCII（minijson.quote 把非 ASCII 转成 \uXXXX），
    # 用 byte_len 而不是字符数，保证多字节场景也符合 Content-Length 语义。
    frame: str = "Content-Length: " + str(minijson.byte_len(body)) + "\r\n\r\n" + body
    sys.stdout.write(frame)
    sys.stdout.flush()

def rpc_result(id_text: str, result_text: str) -> str:
    return '{"jsonrpc":"2.0","id":' + id_text + ',"result":' + result_text + '}'

def rpc_error(id_text: str, code: int, message: str) -> str:
    return '{"jsonrpc":"2.0","id":' + id_text + ',"error":{"code":' + str(code) + ',"message":' + minijson.quote(message) + '}}'

def rpc_notification(method: str, params_text: str) -> str:
    return '{"jsonrpc":"2.0","method":' + minijson.quote(method) + ',"params":' + params_text + '}'

def pos_json(line: int, character: int) -> str:
    return '{"line":' + str(line) + ',"character":' + str(character) + '}'

def range_json(sl: int, sc: int, el: int, ec: int) -> str:
    return '{"start":' + pos_json(sl, sc) + ',"end":' + pos_json(el, ec) + '}'

# ---------------- 文档表（URI -> 文本/版本） ----------------

doc_uris: list[str] = []
doc_texts: list[str] = []
doc_versions: list[int] = []

def doc_index(uri: str) -> int:
    i: int = 0
    while i < len(doc_uris):
        if doc_uris[i] == uri:
            return i
        i = i + 1
    return -1

def doc_put(uri: str, text: str, version: int) -> None:
    i: int = doc_index(uri)
    if i < 0:
        doc_uris.append(uri)
        doc_texts.append(text)
        doc_versions.append(version)
        return
    doc_texts[i] = text
    doc_versions[i] = version

def doc_remove(uri: str) -> None:
    i: int = doc_index(uri)
    if i < 0:
        return
    doc_uris.pop(i)
    doc_texts.pop(i)
    doc_versions.pop(i)

def utf16_units(c: str) -> int:
    if ord(c) > 0xFFFF:
        return 2
    return 1

def cp_index_at(line: str, character: int) -> int:
    # LSP position 的 character 按 UTF-16 code unit 计；转成码点下标
    i: int = 0
    units: int = 0
    while i < len(line) and units < character:
        units = units + utf16_units(line[i])
        i = i + 1
    return i

def offset_at(text: str, line: int, character: int) -> int:
    # (line, character[UTF-16 units]) -> 码点偏移；越界时夹到行尾/文末
    line_no: int = 0
    i: int = 0
    while i < len(text) and line_no < line:
        if text[i] == "\n":
            line_no = line_no + 1
        i = i + 1
    units: int = 0
    while i < len(text) and text[i] != "\n" and units < character:
        units = units + utf16_units(text[i])
        i = i + 1
    return i

def apply_change(text: str, change: minijson.JVal) -> str:
    new_text: str = minijson.get_str(change, "text")
    if not minijson.has(change, "range"):
        return new_text
    rng: minijson.JVal = minijson.get_obj(change, "range")
    st: minijson.JVal = minijson.get_obj(rng, "start")
    en: minijson.JVal = minijson.get_obj(rng, "end")
    a: int = offset_at(text, minijson.get_int(st, "line"), minijson.get_int(st, "character"))
    b: int = offset_at(text, minijson.get_int(en, "line"), minijson.get_int(en, "character"))
    if b < a:
        b = a
    return text[:a] + new_text + text[b:]

# ---------------- 诊断（subprocess 调 pyadamas check） ----------------

def parse_compiler_diagnostics(stderr_text: str, tmp_path: str) -> list[str]:
    out: list[str] = []
    prefix: str = tmp_path + ":"
    lines: list[str] = stderr_text.split("\n")
    i: int = 0
    while i < len(lines):
        raw: str = lines[i]
        i = i + 1
        stripped: str = raw.strip()
        if not stripped.startswith(prefix):
            continue
        rest: str = stripped[len(prefix):]
        p1: int = rest.find(":")
        if p1 < 0:
            continue
        lnum_s: str = rest[:p1]
        rest = rest[p1 + 1:]
        p2: int = rest.find(":")
        if p2 < 0:
            continue
        cnum_s: str = rest[:p2]
        rest = rest[p2 + 1:].strip()
        if not is_digit_str(lnum_s) or not is_digit_str(cnum_s):
            continue
        severity: int = 1
        msg: str = rest
        if rest.startswith("error:"):
            msg = rest[len("error:"):].strip()
        elif rest.startswith("warning:"):
            severity = 2
            msg = rest[len("warning:"):].strip()
        elif rest.startswith("note:"):
            severity = 3
            msg = rest[len("note:"):].strip()
        else:
            continue
        line0: int = int(lnum_s) - 1
        col0: int = int(cnum_s) - 1
        if line0 < 0:
            line0 = 0
        if col0 < 0:
            col0 = 0
        out.append('{"range":' + range_json(line0, col0, line0, col0 + 1) + ',"severity":' + str(severity) + ',"source":"pyadamas","message":' + minijson.quote(msg) + '}')
    return out

def compute_diagnostics(text: str) -> list[str]:
    tmp: str = temp_file_path()
    if not write_file(tmp, text):
        log("cannot write temp file " + tmp)
        return []
    stderr_text: str = ""
    try:
        r = subprocess.run([compiler_path(), "check", tmp], capture_output=True, text=True, timeout=30.0)
        stderr_text = r.stderr
        log("check rc=" + str(r.returncode) + " stderr-bytes=" + str(len(stderr_text)))
    except TimeoutExpired:
        log("pyadamas check timed out")
        stderr_text = ""
    remove_file(tmp)
    return parse_compiler_diagnostics(stderr_text, tmp)

def publish_diagnostics(uri: str, text: str, version: int) -> None:
    diags: list[str] = compute_diagnostics(text)
    params: str = '{"uri":' + minijson.quote(uri) + ',"version":' + str(version) + ',"diagnostics":[' + ",".join(diags) + ']}'
    send_frame(rpc_notification("textDocument/publishDiagnostics", params))
    log("published " + str(len(diags)) + " diagnostic(s) for " + uri)

# ---------------- documentSymbol：顶层 def / class / 赋值 ----------------

class Symbol:
    kind: int
    name: str
    line: int
    col: int
    end_line: int
    end_col: int

    def __init__(self) -> None:
        self.kind = 0
        self.name = ""
        self.line = 0
        self.col = 0
        self.end_line = 0
        self.end_col = 0

def scan_symbols(text: str) -> list[Symbol]:
    syms: list[Symbol] = []
    lines: list[str] = text.split("\n")
    n: int = len(lines)
    i: int = 0
    while i < n:
        raw: str = lines[i]
        stripped: str = raw.strip()
        if stripped == "" or stripped.startswith("#"):
            i = i + 1
            continue
        if indent_width(raw) > 0:
            i = i + 1
            continue
        sym: Symbol = Symbol()
        sym.line = i
        sym.end_line = i
        sym.end_col = len(raw)
        found: bool = False
        if stripped.startswith("async def "):
            sym.kind = 12
            sym.name = read_ident(stripped, len("async def "))
            sym.col = len("async def ")
            found = sym.name != ""
        elif stripped.startswith("def "):
            sym.kind = 12
            sym.name = read_ident(stripped, len("def "))
            sym.col = len("def ")
            found = sym.name != ""
        elif stripped.startswith("class "):
            sym.kind = 5
            sym.name = read_ident(stripped, len("class "))
            sym.col = len("class ")
            found = sym.name != ""
        else:
            nm: str = read_ident(raw, 0)
            if nm != "":
                pos: int = first_non_space(raw, len(nm))
                if pos < len(raw) and raw[pos] == ":":
                    if raw.find("=", pos + 1) >= 0:
                        sym.kind = 13
                        sym.name = nm
                        found = True
                elif pos < len(raw) and raw[pos] == "=":
                    if pos + 1 >= len(raw) or raw[pos + 1] != "=":
                        sym.kind = 13
                        sym.name = nm
                        found = True
        if not found:
            i = i + 1
            continue
        if sym.kind == 12 or sym.kind == 5:
            # 块尾：往后扫到下一个顶层非空、非注释行为止
            j: int = i + 1
            last: int = i
            while j < n:
                s2: str = lines[j].strip()
                if s2 == "":
                    j = j + 1
                    continue
                if s2.startswith("#") and indent_width(lines[j]) == 0:
                    j = j + 1
                    continue
                if indent_width(lines[j]) == 0:
                    break
                last = j
                j = j + 1
            sym.end_line = last
            sym.end_col = len(lines[last])
        syms.append(sym)
        i = i + 1
    return syms

def symbols_json(syms: list[Symbol]) -> str:
    parts: list[str] = []
    i: int = 0
    while i < len(syms):
        s: Symbol = syms[i]
        parts.append('{"name":' + minijson.quote(s.name) + ',"kind":' + str(s.kind) + ',"range":' + range_json(s.line, s.col, s.end_line, s.end_col) + ',"selectionRange":' + range_json(s.line, s.col, s.line, s.col + len(s.name)) + '}')
        i = i + 1
    return "[" + ",".join(parts) + "]"

# ---------------- hover：光标标识符 -> 定义处签名/类型 ----------------

def word_at(text: str, line: int, character: int) -> str:
    lines: list[str] = text.split("\n")
    if line < 0 or line >= len(lines):
        return ""
    raw: str = lines[line]
    idx: int = cp_index_at(raw, character)
    start: int = idx
    while start > 0 and is_ident_char(raw[start - 1]):
        start = start - 1
    end: int = idx
    while end < len(raw) and is_ident_char(raw[end]):
        end = end + 1
    if end <= start:
        return ""
    return raw[start:end]

def params_text(sig: str) -> str:
    p1: int = sig.find("(")
    p2: int = sig.find(")")
    if p1 < 0 or p2 < 0 or p2 <= p1:
        return ""
    return sig[p1 + 1:p2]

def is_param(params: str, word: str) -> bool:
    parts: list[str] = params.split(",")
    i: int = 0
    while i < len(parts):
        p: str = parts[i].strip()
        cut: int = len(p)
        c1: int = p.find(":")
        c2: int = p.find("=")
        if c1 >= 0 and c1 < cut:
            cut = c1
        if c2 >= 0 and c2 < cut:
            cut = c2
        nm: str = p[:cut].strip()
        while len(nm) > 0 and (nm[0] == "*" or nm[0] == "/"):
            nm = nm[1:]
        if nm == word:
            return True
        i = i + 1
    return False

def find_def_line(text: str, word: str) -> str:
    lines: list[str] = text.split("\n")
    i: int = 0
    while i < len(lines):
        stripped: str = lines[i].strip()
        if stripped.startswith("async def "):
            sig: str = strip_trailing_colon(stripped)
            if read_ident(stripped, len("async def ")) == word:
                return sig
            if is_param(params_text(sig), word):
                return sig
        elif stripped.startswith("def "):
            sig2: str = strip_trailing_colon(stripped)
            if read_ident(stripped, len("def ")) == word:
                return sig2
            if is_param(params_text(sig2), word):
                return sig2
        elif stripped.startswith("class "):
            if read_ident(stripped, len("class ")) == word:
                return strip_trailing_colon(stripped)
        else:
            nm: str = read_ident(stripped, 0)
            if nm == word:
                pos: int = first_non_space(stripped, len(nm))
                if pos < len(stripped):
                    c: str = stripped[pos]
                    if c == ":" or c == "=":
                        return truncate(stripped, 160)
        i = i + 1
    return ""

# ---------------- 各方法处理 ----------------

def init_result() -> str:
    caps: str = '{"textDocumentSync":{"openClose":true,"change":1},"hoverProvider":true,"documentSymbolProvider":true}'
    return '{"capabilities":' + caps + ',"serverInfo":{"name":"pya-lsp","version":"0.1.0"}}'

def params_of(msg: minijson.JVal) -> minijson.JVal:
    return minijson.get_obj(msg, "params")

def uri_of(params: minijson.JVal) -> str:
    td: minijson.JVal = minijson.get_obj(params, "textDocument")
    return minijson.get_str(td, "uri")

def handle_did_open(params: minijson.JVal) -> None:
    td: minijson.JVal = minijson.get_obj(params, "textDocument")
    uri: str = minijson.get_str(td, "uri")
    text: str = minijson.get_str(td, "text")
    version: int = minijson.get_int(td, "version")
    if uri == "":
        return
    doc_put(uri, text, version)
    publish_diagnostics(uri, text, version)

def handle_did_change(params: minijson.JVal) -> None:
    td: minijson.JVal = minijson.get_obj(params, "textDocument")
    uri: str = minijson.get_str(td, "uri")
    version: int = minijson.get_int(td, "version")
    if uri == "":
        return
    idx: int = doc_index(uri)
    text: str = ""
    if idx >= 0:
        text = doc_texts[idx]
    changes: minijson.JVal = minijson.get_arr(params, "contentChanges")
    count: int = minijson.size(changes)
    k: int = 0
    while k < count:
        text = apply_change(text, minijson.at(changes, k))
        k = k + 1
    doc_put(uri, text, version)
    publish_diagnostics(uri, text, version)

def handle_did_close(params: minijson.JVal) -> None:
    uri: str = uri_of(params)
    if uri == "":
        return
    doc_remove(uri)
    clear: str = '{"uri":' + minijson.quote(uri) + ',"diagnostics":[]}'
    send_frame(rpc_notification("textDocument/publishDiagnostics", clear))

def handle_document_symbol(id_text: str, params: minijson.JVal) -> None:
    uri: str = uri_of(params)
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    syms: list[Symbol] = scan_symbols(doc_texts[idx])
    send_frame(rpc_result(id_text, symbols_json(syms)))

def handle_hover(id_text: str, params: minijson.JVal) -> None:
    uri: str = uri_of(params)
    pos: minijson.JVal = minijson.get_obj(params, "position")
    line: int = minijson.get_int(pos, "line")
    character: int = minijson.get_int(pos, "character")
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    text: str = doc_texts[idx]
    word: str = word_at(text, line, character)
    if word == "":
        send_frame(rpc_result(id_text, "null"))
        return
    sig: str = find_def_line(text, word)
    if sig == "":
        send_frame(rpc_result(id_text, "null"))
        return
    hover: str = '{"contents":{"kind":"plaintext","value":' + minijson.quote(sig) + '}}'
    send_frame(rpc_result(id_text, hover))

# ---------------- 主循环 ----------------

def run() -> int:
    shutdown_requested: bool = False
    inp: file = open_stdin()
    while True:
        body: str = read_frame(inp)
        if body == "":
            log("stdin closed, exiting")
            return 0
        log("recv body bytes=" + str(len(body)))
        msg: minijson.JVal = minijson.new_null()
        try:
            msg = minijson.parse(body)
        except ValueError:
            log("malformed JSON, ignored")
            continue
        method: str = minijson.get_str(msg, "method")
        if method == "":
            # 客户端对服务端请求的响应：本服务器不发请求，忽略
            continue
        has_id: bool = minijson.has(msg, "id")
        id_text: str = "null"
        if has_id:
            id_text = minijson.stringify(minijson.get(msg, "id"))
        params: minijson.JVal = params_of(msg)
        log("method=" + method + " id=" + id_text)
        if method == "initialize":
            send_frame(rpc_result(id_text, init_result()))
        elif method == "initialized":
            pass
        elif method == "shutdown":
            shutdown_requested = True
            send_frame(rpc_result(id_text, "null"))
        elif method == "exit":
            log("exit notification")
            if shutdown_requested:
                return 0
            return 1
        elif shutdown_requested:
            if has_id:
                send_frame(rpc_error(id_text, -32600, "server is shutting down"))
        elif method == "textDocument/didOpen":
            handle_did_open(params)
        elif method == "textDocument/didChange":
            handle_did_change(params)
        elif method == "textDocument/didClose":
            handle_did_close(params)
        elif method == "textDocument/documentSymbol":
            handle_document_symbol(id_text, params)
        elif method == "textDocument/hover":
            handle_hover(id_text, params)
        else:
            if has_id:
                send_frame(rpc_error(id_text, -32601, "method not found: " + method))

def main() -> None:
    sys.exit(run())

main()
