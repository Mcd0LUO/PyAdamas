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
import asyncio
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
    # 二进制帧读取：Content-Length 是字节数，帧体是 UTF-8，必须按字节读。
    # 0.447 修好了流式语义（非普通文件上一次 read(2)，不凑满 8192）：
    #   * 文本模式 sys.stdin 现在也能「有多少拿多少」，read(n) 按 UTF-8 字节
    #     计（实测 "hélloXX" read(6) 得到 6 字节）；
    #   * 但 LSP 帧是二进制语义，保留 open(0, "rb") 更稳（不依赖 stdin 的
    #     文本缓冲/换行翻译），也避免某些客户端发 CRLF 边界时被翻译。
    # buffering=0（无缓冲）：fd 层可读性 == 是否有待处理数据，为后续
    # didChange 合并（poll 一秒内是否有下一帧）留出可靠依据；每帧只有
    # 几百字节，逐字节 read(2) 的开销可以忽略。
    # 保底：/dev/stdin（部分平台 fd 0 不可 open）→ sys.stdin（最后手段）。
    try:
        return open(0, "rb", 0)
    except OSError:
        log("open(0, 'rb') failed, trying /dev/stdin")
        try:
            return open("/dev/stdin", "rb")
        except OSError:
            log("cannot open binary stdin, falling back to sys.stdin")
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

def apply_change(text: str, change: unknown) -> str:
    new_text: str = minijson.get_str(change, "text")
    if not minijson.has(change, "range"):
        return new_text
    rng: unknown = minijson.get_obj(change, "range")
    st: unknown = minijson.get_obj(rng, "start")
    en: unknown = minijson.get_obj(rng, "end")
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


# ---------------- 自动补全：文档符号 + import 成员 + Python 关键字 ----------------

# CompletionItemKind（LSP 规范）：1 Text / 2 Method / 3 Function / 5 Field /
# 6 Variable / 7 Class / 9 Module / 14 Keyword。
comp_kind_function: int = 3
comp_kind_class: int = 7
comp_kind_variable: int = 6
comp_kind_module: int = 9
comp_kind_keyword: int = 14

PY_KEYWORDS: list[str] = [
    "False", "None", "True", "and", "as", "assert", "async", "await", "break",
    "class", "continue", "def", "del", "elif", "else", "except", "finally",
    "for", "from", "global", "if", "import", "in", "is", "lambda", "nonlocal",
    "not", "or", "pass", "raise", "return", "try", "while", "with", "yield",
]

# 内置模块的常用成员（补全候选用）。只列容易记错的常用面，不求全：
# 未知模块/成员可以照常手写，补全只是辅助。
MODULE_MEMBERS: dict[str, list[str]] = {
    "math": ["sqrt", "floor", "ceil", "fabs", "sin", "cos", "tan", "asin", "acos",
             "atan", "atan2", "log", "log2", "log10", "exp", "pow", "isnan",
             "isinf", "isfinite", "gcd", "lcm", "factorial", "isqrt", "hypot",
             "pi", "e", "tau", "inf", "nan", "fsum", "prod"],
    "sys": ["argv", "exit", "maxsize", "stdin", "stdout", "stderr", "executable"],
    "os": ["getcwd", "mkdir", "makedirs", "remove", "rename", "rmdir", "getenv",
           "cpu_count", "path", "environ"],
    "os.path": ["join", "basename", "dirname", "splitext", "exists", "isdir",
                "isfile", "abspath"],
    "json": ["parse", "stringify", "dumps", "loads", "get", "get_or", "has",
             "path", "len", "keys", "type", "is_null", "as_str", "as_int",
             "as_float", "as_bool", "as_dict", "as_list", "null", "bool"],
    "re": ["compile", "search", "match", "fullmatch", "findall", "finditer",
           "sub", "split", "escape", "I", "IGNORECASE", "M", "MULTILINE",
           "S", "DOTALL"],
    "collections": ["deque", "Counter", "OrderedDict", "defaultdict"],
    "itertools": ["chain", "islice", "product", "permutations", "combinations",
                  "repeat", "zip_longest", "accumulate"],
    "functools": ["reduce", "wraps", "partial", "lru_cache"],
    "datetime": ["datetime", "date", "timedelta"],
    "pathlib": ["Path"],
    "time": ["time", "monotonic", "perf_counter", "sleep"],
    "subprocess": ["run", "Popen", "PIPE", "DEVNULL", "STDOUT"],
    "random": ["random", "seed", "randint", "uniform", "choice", "shuffle",
               "randrange", "getrandbits", "gauss"],
    "asyncio": ["run", "sleep", "gather", "create_task", "wait_for",
                "wait_readable", "Future", "Event", "Lock", "Queue"],
    "base64": ["b64encode", "b64decode", "urlsafe_b64encode",
               "urlsafe_b64decode"],
    "hashlib": ["sha256", "md5", "new"],
    "logging": ["getLogger", "basicConfig", "DEBUG", "INFO", "WARNING",
                "ERROR", "CRITICAL"],
    "string": ["ascii_letters", "ascii_lowercase", "ascii_uppercase", "digits",
               "punctuation"],
}

def list_has(xs: list[str], s: str) -> bool:
    i: int = 0
    while i < len(xs):
        if xs[i] == s:
            return True
        i = i + 1
    return False

def is_py_keyword(w: str) -> bool:
    return list_has(PY_KEYWORDS, w)

def add_candidate(labels: list[str], kinds: list[int], details: list[str],
                  label: str, kind: int, detail: str) -> None:
    if label == "" or list_has(labels, label):
        return
    labels.append(label)
    kinds.append(kind)
    details.append(detail)

def symbol_detail(lines: list[str], s: Symbol) -> str:
    if s.line < 0 or s.line >= len(lines):
        return ""
    raw: str = lines[s.line].strip()
    if s.kind == 12 or s.kind == 5:
        return truncate(strip_trailing_colon(raw), 120)
    return truncate(raw, 120)


def collect_completions(text: str, line: int, character: int) -> str:
    lines: list[str] = text.split("\n")
    prefix: str = ""
    if line >= 0 and line < len(lines):
        raw_line: str = lines[line]
        idx: int = cp_index_at(raw_line, character)
        start: int = idx
        while start > 0 and is_ident_char(raw_line[start - 1]):
            start = start - 1
        prefix = raw_line[start:idx]
    labels: list[str] = []
    kinds: list[int] = []
    details: list[str] = []
    # 1) 当前文档的顶层符号
    syms: list[Symbol] = scan_symbols(text)
    i: int = 0
    while i < len(syms):
        s: Symbol = syms[i]
        if s.kind == 12:
            add_candidate(labels, kinds, details, s.name, comp_kind_function,
                          symbol_detail(lines, s))
        elif s.kind == 5:
            add_candidate(labels, kinds, details, s.name, comp_kind_class,
                          symbol_detail(lines, s))
        else:
            add_candidate(labels, kinds, details, s.name, comp_kind_variable,
                          symbol_detail(lines, s))
        i = i + 1
    # 2) import 过的模块成员 / from-import 的名字
    i = 0
    while i < len(lines):
        stripped: str = lines[i].strip()
        if stripped.startswith("import "):
            rest: str = stripped[len("import "):].strip()
            mods: list[str] = rest.split(",")
            k: int = 0
            while k < len(mods):
                mod: str = mods[k].strip()
                aspos: int = mod.find(" as ")
                if aspos >= 0:
                    mod = mod[:aspos].strip()
                if mod != "":
                    # 模块名本身也是候选（写 \`math.\` 之前先补模块名）
                    add_candidate(labels, kinds, details, mod, comp_kind_module,
                                  "module")
                if mod in MODULE_MEMBERS:
                    members: list[str] = MODULE_MEMBERS[mod]
                    m: int = 0
                    while m < len(members):
                        add_candidate(labels, kinds, details, members[m],
                                      comp_kind_module, "import " + mod)
                        m = m + 1
                k = k + 1
        elif stripped.startswith("from "):
            rest2: str = stripped[len("from "):].strip()
            ipos: int = rest2.find(" import ")
            if ipos >= 0:
                mod2: str = rest2[:ipos].strip()
                names: str = rest2[ipos + len(" import "):].strip()
                parts: list[str] = names.split(",")
                k2: int = 0
                while k2 < len(parts):
                    nm: str = parts[k2].strip()
                    aspos2: int = nm.find(" as ")
                    if aspos2 >= 0:
                        nm = nm[:aspos2].strip()
                    if nm != "*" and nm != "" and not nm.startswith("("):
                        add_candidate(labels, kinds, details, nm,
                                      comp_kind_module,
                                      "from " + mod2 + " import")
                    k2 = k2 + 1
                if mod2 in MODULE_MEMBERS:
                    # 允许「from math import <tab>」直接给模块成员
                    members2: list[str] = MODULE_MEMBERS[mod2]
                    m2: int = 0
                    while m2 < len(members2):
                        add_candidate(labels, kinds, details, members2[m2],
                                      comp_kind_module, "from " + mod2)
                        m2 = m2 + 1
        i = i + 1
    # 3) Python 关键字
    i = 0
    while i < len(PY_KEYWORDS):
        add_candidate(labels, kinds, details, PY_KEYWORDS[i], comp_kind_keyword,
                      "keyword")
        i = i + 1
    # 4) 前缀过滤 + 上限 100
    items: list[str] = []
    i = 0
    while i < len(labels) and len(items) < 100:
        if labels[i].startswith(prefix):
            item: str = ('{"label":' + minijson.quote(labels[i]) +
                         ',"kind":' + str(kinds[i]) +
                         ',"detail":' + minijson.quote(details[i]) + '}')
            items.append(item)
        i = i + 1
    log("completion prefix=" + prefix + " items=" + str(len(items)))
    return "[" + ",".join(items) + "]"

def handle_completion(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    pos: unknown = minijson.get_obj(params, "position")
    line: int = minijson.get_int(pos, "line")
    character: int = minijson.get_int(pos, "character")
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "[]"))
        return
    send_frame(rpc_result(id_text,
                          collect_completions(doc_texts[idx], line, character)))

# ---------------- 跳转定义：标识符 -> 定义位置 ----------------

def leading_ws(raw: str) -> int:
    i: int = 0
    while i < len(raw) and (raw[i] == " " or raw[i] == "\t"):
        i = i + 1
    return i

def find_def_location(text: str, word: str) -> list[int]:
    # 返回 [line, character]（UTF-16 列，标识符都是 ASCII 所以等于码点数），
    # 找不到返回 []。覆盖：任意缩进的 def/async def/class/方法、顶层赋值、
    # 以及「光标在形参上 -> 跳到所在函数定义」。
    lines: list[str] = text.split("\n")
    i: int = 0
    while i < len(lines):
        raw: str = lines[i]
        indent: int = leading_ws(raw)
        stripped: str = raw[indent:]
        # 形参命中：先匹配函数名，再匹配形参名（跳函数定义）
        if stripped.startswith("async def ") or stripped.startswith("def "):
            off: int = len("async def ")
            if not stripped.startswith("async def "):
                off = len("def ")
            fname: str = read_ident(stripped, off)
            sig: str = strip_trailing_colon(stripped)
            if fname == word:
                return [i, indent + off]
            if is_param(params_text(sig), word) and fname != "":
                return [i, indent + off]
        elif stripped.startswith("class "):
            cname: str = read_ident(stripped, len("class "))
            if cname == word:
                return [i, indent + len("class ")]
        elif indent == 0:
            nm: str = read_ident(stripped, 0)
            if nm == word:
                pos: int = first_non_space(stripped, len(nm))
                if pos < len(stripped) and (stripped[pos] == ":" or stripped[pos] == "="):
                    return [i, 0]
        i = i + 1
    return []

def location_json(uri: str, line: int, character: int, length: int) -> str:
    return ('{"uri":' + minijson.quote(uri) + ',"range":' +
            range_json(line, character, line, character + length) + '}')

def handle_definition(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    pos: unknown = minijson.get_obj(params, "position")
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
    loc: list[int] = find_def_location(text, word)
    if len(loc) < 2:
        send_frame(rpc_result(id_text, "null"))
        return
    send_frame(rpc_result(id_text, location_json(uri, loc[0], loc[1], len(word))))

# ---------------- 语义高亮：semanticTokens/full ----------------
#
# legend 顺序决定 tokenType 索引（initialize 里回给客户端同一份）。
# 只做单行 token：注释到行尾、普通引号字符串（含转义）、数字、关键字、
# 类名/函数名/其它标识符。三引号跨行字符串按行内字符串近似（README 说明）。
TOKEN_CLASS: int = 0
TOKEN_FUNCTION: int = 1
TOKEN_VARIABLE: int = 2
TOKEN_KEYWORD: int = 3
TOKEN_STRING: int = 4
TOKEN_NUMBER: int = 5
TOKEN_COMMENT: int = 6
TOKEN_PARAMETER: int = 7

TOKEN_TYPES: list[str] = ["class", "function", "variable", "keyword", "string",
                          "number", "comment", "parameter"]

def u16_prefix_len(s: str, idx: int) -> int:
    # 码点下标 -> UTF-16 code unit 数（LSP position 的口径）。
    units: int = 0
    i: int = 0
    while i < idx and i < len(s):
        units = units + utf16_units(s[i])
        i = i + 1
    return units

def is_digit_char(c: str) -> bool:
    return c >= "0" and c <= "9"

class TokenEmitter:
    data: list[str]
    prev_line: int
    prev_start: int

    def __init__(self) -> None:
        self.data = []
        self.prev_line = 0
        self.prev_start = 0

    def emit(self, line: int, raw: str, start: int, end: int, ttype: int) -> None:
        if end <= start or start < 0 or end > len(raw):
            return
        start_u16: int = u16_prefix_len(raw, start)
        end_u16: int = u16_prefix_len(raw, end)
        length: int = end_u16 - start_u16
        if length <= 0:
            return
        delta_line: int = line - self.prev_line
        if delta_line == 0:
            delta_start: int = start_u16 - self.prev_start
        else:
            delta_start = start_u16
        self.data.append(str(delta_line) + "," + str(delta_start) + "," +
                         str(length) + "," + str(ttype) + ",0")
        self.prev_line = line
        self.prev_start = start_u16

def class_name_set(text: str) -> list[str]:
    names: list[str] = []
    syms: list[Symbol] = scan_symbols(text)
    i: int = 0
    while i < len(syms):
        if syms[i].kind == 5:
            names.append(syms[i].name)
        i = i + 1
    return names

def func_name_set(text: str) -> list[str]:
    names: list[str] = []
    syms: list[Symbol] = scan_symbols(text)
    i: int = 0
    while i < len(syms):
        if syms[i].kind == 12:
            names.append(syms[i].name)
        i = i + 1
    return names

def scan_ident_end(s: str, start: int) -> int:
    i: int = start
    while i < len(s) and is_ident_char(s[i]):
        i = i + 1
    return i

def semantic_tokens(text: str) -> str:
    lines: list[str] = text.split("\n")
    classes: list[str] = class_name_set(text)
    funcs: list[str] = func_name_set(text)
    emitter: TokenEmitter = TokenEmitter()
    line_no: int = 0
    triple: str = ""   # 跨行三引号字符串的状态（"\"\"\"" / "'''"）
    while line_no < len(lines):
        raw: str = lines[line_no]
        i: int = 0
        if triple != "":
            # 上一行还没闭合的三引号字符串：找本行内的闭合位置
            close: int = raw.find(triple)
            if close < 0:
                emitter.emit(line_no, raw, 0, len(raw), TOKEN_STRING)
                line_no = line_no + 1
                continue
            emitter.emit(line_no, raw, close, close + 3, TOKEN_STRING)
            i = close + 3
            triple = ""
        while i < len(raw):
            c: str = raw[i]
            if c == "#":
                emitter.emit(line_no, raw, i, len(raw), TOKEN_COMMENT)
                break
            if raw[i:i + 3] == '"""' or raw[i:i + 3] == "'''":
                q3: str = raw[i:i + 3]
                close2: int = raw.find(q3, i + 3)
                if close2 < 0:
                    emitter.emit(line_no, raw, i, len(raw), TOKEN_STRING)
                    triple = q3
                    break
                emitter.emit(line_no, raw, i, close2 + 3, TOKEN_STRING)
                i = close2 + 3
                continue
            if c == '"' or c == "'":
                q: str = c
                j: int = i + 1
                closed: bool = False
                while j < len(raw):
                    if raw[j] == "\\":
                        j = j + 2
                        continue
                    if raw[j] == q:
                        j = j + 1
                        closed = True
                        break
                    j = j + 1
                if j > len(raw):
                    j = len(raw)
                emitter.emit(line_no, raw, i, j, TOKEN_STRING)
                i = j
                continue
            if is_digit_char(c) and (i == 0 or not is_ident_char(raw[i - 1])):
                j2: int = i
                two: str = raw[i:i + 2]
                if two == "0x" or two == "0X" or two == "0b" or two == "0B" or two == "0o" or two == "0O":
                    # 进制前缀：吞掉前缀与后续字母数字/下划线
                    j2 = i + 2
                    while j2 < len(raw) and (is_ident_char(raw[j2]) or raw[j2] == "_"):
                        j2 = j2 + 1
                else:
                    while j2 < len(raw) and (is_digit_char(raw[j2]) or raw[j2] == "_"):
                        j2 = j2 + 1
                    if j2 < len(raw) and raw[j2] == ".":
                        j2 = j2 + 1
                        while j2 < len(raw) and is_digit_char(raw[j2]):
                            j2 = j2 + 1
                    if j2 < len(raw) and (raw[j2] == "e" or raw[j2] == "E"):
                        k: int = j2 + 1
                        if k < len(raw) and (raw[k] == "+" or raw[k] == "-"):
                            k = k + 1
                        if k < len(raw) and is_digit_char(raw[k]):
                            j2 = k + 1
                            while j2 < len(raw) and is_digit_char(raw[j2]):
                                j2 = j2 + 1
                emitter.emit(line_no, raw, i, j2, TOKEN_NUMBER)
                i = j2
                continue
            if is_ident_char(c) and (i == 0 or not is_ident_char(raw[i - 1])):
                j3: int = scan_ident_end(raw, i)
                word: str = raw[i:j3]
                ttype: int = TOKEN_VARIABLE
                if is_py_keyword(word):
                    ttype = TOKEN_KEYWORD
                elif list_has(classes, word):
                    ttype = TOKEN_CLASS
                elif list_has(funcs, word):
                    ttype = TOKEN_FUNCTION
                emitter.emit(line_no, raw, i, j3, ttype)
                i = j3
                continue
            i = i + 1
        line_no = line_no + 1
    return "{" + '"data":[' + ",".join(emitter.data) + "]}"

def handle_semantic_tokens(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "{\"data\":[]}"))
        return
    send_frame(rpc_result(id_text, semantic_tokens(doc_texts[idx])))

# ---------------- signatureHelp：调用括号内给参数提示 ----------------
# 设计：从文件头正向扫描到光标，用括号栈找「包含光标的最内层 (」——这样跨行调用、
# 嵌套调用、字符串/注释里的括号都能正确处理；再取 '(' 前的标识符，复用 hover 的
# 定义行查找（find_def_line）拿签名。activeParameter = 该 '(' 到光标之间、括号深度
# 为 0 的逗号个数（跳过字符串/注释里的逗号）。

def find_enclosing_paren(text: str, cursor: int) -> int:
    # 返回包含 cursor 的最内层 '(' 的码点偏移；没有返回 -1。
    # 只跳过注释/普通字符串/三引号字符串（f-string 内部表达式按字符串近似）。
    stack: list[int] = []
    i: int = 0
    triple: str = ""
    while i < cursor:
        c: str = text[i]
        if triple != "":
            if c == "\\":
                i = i + 2
                continue
            if text[i:i + 3] == triple:
                triple = ""
                i = i + 3
                continue
            i = i + 1
            continue
        if c == "#":
            nl: int = text.find("\n", i)
            if nl < 0:
                i = cursor
            else:
                i = nl + 1
            continue
        if text[i:i + 3] == '"""' or text[i:i + 3] == "'''":
            triple = text[i:i + 3]
            i = i + 3
            continue
        if c == '"' or c == "'":
            q: str = c
            i = i + 1
            while i < cursor:
                ch: str = text[i]
                if ch == "\\":
                    i = i + 2
                    continue
                if ch == q:
                    i = i + 1
                    break
                i = i + 1
            continue
        if c == "(":
            stack.append(i)
        elif c == ")":
            if len(stack) > 0:
                stack.pop()
        i = i + 1
    if len(stack) == 0:
        return -1
    return stack[len(stack) - 1]

def callee_before_paren(text: str, paren: int) -> list[int]:
    # '(' 前（跳过空白）的标识符 [start, end]；不是标识符返回 []。
    j: int = paren - 1
    while j >= 0 and (text[j] == " " or text[j] == "\t" or text[j] == "\n" or text[j] == "\r"):
        j = j - 1
    if j < 0 or not is_ident_char(text[j]):
        return []
    end: int = j + 1
    while j >= 0 and is_ident_char(text[j]):
        j = j - 1
    return [j + 1, end]

def count_call_commas(text: str, paren: int, cursor: int) -> int:
    # paren+1 .. cursor 之间、括号深度 0 的逗号数；跳过字符串/注释/嵌套括号/列表字典。
    depth: int = 0
    commas: int = 0
    i: int = paren + 1
    triple: str = ""
    while i < cursor:
        c: str = text[i]
        if triple != "":
            if c == "\\":
                i = i + 2
                continue
            if text[i:i + 3] == triple:
                triple = ""
                i = i + 3
                continue
            i = i + 1
            continue
        if c == "#":
            nl: int = text.find("\n", i)
            if nl < 0 or nl >= cursor:
                return commas
            i = nl + 1
            continue
        if text[i:i + 3] == '"""' or text[i:i + 3] == "'''":
            triple = text[i:i + 3]
            i = i + 3
            continue
        if c == '"' or c == "'":
            q: str = c
            i = i + 1
            while i < cursor:
                ch: str = text[i]
                if ch == "\\":
                    i = i + 2
                    continue
                if ch == q:
                    i = i + 1
                    break
                i = i + 1
            continue
        if c == "(" or c == "[" or c == "{":
            depth = depth + 1
        elif c == ")" or c == "]" or c == "}":
            if depth > 0:
                depth = depth - 1
        elif c == "," and depth == 0:
            commas = commas + 1
        i = i + 1
    return commas

def signature_param_labels(sig: str) -> list[str]:
    # 从定义行取参数的展示文本；跳过单独的位置/关键字参数分隔符 * 与 /。
    inner: str = params_text(sig)
    labels: list[str] = []
    if inner.strip() == "":
        return labels
    parts: list[str] = inner.split(",")
    i: int = 0
    while i < len(parts):
        p: str = parts[i].strip()
        if p != "" and p != "*" and p != "/":
            labels.append(p)
        i = i + 1
    return labels

def signature_label(sig: str) -> str:
    # 展示用：去掉开头的 def/async def，保留 名字(参数) -> 返回类型。
    if sig.startswith("async def "):
        return sig[len("async def "):]
    if sig.startswith("def "):
        return sig[len("def "):]
    return sig

def handle_signature_help(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    pos: unknown = minijson.get_obj(params, "position")
    line: int = minijson.get_int(pos, "line")
    character: int = minijson.get_int(pos, "character")
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    text: str = doc_texts[idx]
    cursor: int = offset_at(text, line, character)
    paren: int = find_enclosing_paren(text, cursor)
    if paren < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    callee: list[int] = callee_before_paren(text, paren)
    if len(callee) < 2:
        send_frame(rpc_result(id_text, "null"))
        return
    name: str = text[callee[0]:callee[1]]
    sig: str = find_def_line(text, name)
    if sig == "":
        send_frame(rpc_result(id_text, "null"))
        return
    labels: list[str] = signature_param_labels(sig)
    active: int = count_call_commas(text, paren, cursor)
    if len(labels) == 0:
        active = 0
    elif active >= len(labels):
        active = len(labels) - 1
    if active < 0:
        active = 0
    params_js: list[str] = []
    i: int = 0
    while i < len(labels):
        params_js.append('{"label":' + minijson.quote(labels[i]) + '}')
        i = i + 1
    body: str = ('{"signatures":[{"label":' + minijson.quote(signature_label(sig)) +
                 ',"parameters":[' + ",".join(params_js) +
                 '],"activeParameter":' + str(active) + '}],' +
                 '"activeSignature":0,"activeParameter":' + str(active) + '}')
    send_frame(rpc_result(id_text, body))

# ---------------- documentHighlight：同名标识符高亮 ----------------
# 自己实现一份扫描（不依赖 references 那边）：逐行扫，跳过注释/字符串/三引号字符串；
# 标识符按整词匹配。kind：定义/赋值目标/形参 -> Write(3)，其余 -> Text(1)。
HIGHLIGHT_TEXT: int = 1
HIGHLIGHT_WRITE: int = 3

def occurrence_kind(stripped: str, raw: str, start: int, end: int) -> int:
    word: str = raw[start:end]
    # def / async def / class 的名字、def 的形参 -> 写
    if stripped.startswith("async def "):
        if read_ident(stripped, len("async def ")) == word:
            return HIGHLIGHT_WRITE
        if is_param(params_text(strip_trailing_colon(stripped)), word):
            return HIGHLIGHT_WRITE
    elif stripped.startswith("def "):
        if read_ident(stripped, len("def ")) == word:
            return HIGHLIGHT_WRITE
        if is_param(params_text(strip_trailing_colon(stripped)), word):
            return HIGHLIGHT_WRITE
    elif stripped.startswith("class "):
        if read_ident(stripped, len("class ")) == word:
            return HIGHLIGHT_WRITE
    # 赋值目标：name [ : 类型 ] = （排除 == / <= / >= / != / += 等）
    pos: int = end
    while pos < len(raw) and (raw[pos] == " " or raw[pos] == "\t"):
        pos = pos + 1
    if pos < len(raw) and raw[pos] == ":":
        eq: int = raw.find("=", pos + 1)
        if eq >= 0:
            return HIGHLIGHT_WRITE
    if pos + 1 < len(raw) and raw[pos] == "=" and raw[pos + 1] != "=":
        return HIGHLIGHT_WRITE
    if pos < len(raw) and raw[pos] == "=" and pos + 1 >= len(raw):
        return HIGHLIGHT_WRITE
    return HIGHLIGHT_TEXT

def highlight_ranges(text: str, word: str) -> str:
    lines: list[str] = text.split("\n")
    parts: list[str] = []
    triple: str = ""
    line_no: int = 0
    while line_no < len(lines):
        raw: str = lines[line_no]
        i: int = 0
        if triple != "":
            close: int = raw.find(triple)
            if close < 0:
                line_no = line_no + 1
                continue
            i = close + 3
            triple = ""
        while i < len(raw):
            c: str = raw[i]
            if c == "#":
                break
            if raw[i:i + 3] == '"""' or raw[i:i + 3] == "'''":
                q3: str = raw[i:i + 3]
                close2: int = raw.find(q3, i + 3)
                if close2 < 0:
                    triple = q3
                    break
                i = close2 + 3
                continue
            if c == '"' or c == "'":
                q: str = c
                j: int = i + 1
                while j < len(raw):
                    if raw[j] == "\\":
                        j = j + 2
                        continue
                    if raw[j] == q:
                        j = j + 1
                        break
                    j = j + 1
                i = j
                continue
            if is_ident_char(c) and (i == 0 or not is_ident_char(raw[i - 1])):
                j2: int = scan_ident_end(raw, i)
                if raw[i:j2] == word:
                    kind: int = occurrence_kind(raw.strip(), raw, i, j2)
                    parts.append('{"range":' + range_json(line_no, u16_prefix_len(raw, i),
                                                           line_no, u16_prefix_len(raw, j2)) +
                                 ',"kind":' + str(kind) + '}')
                i = j2
                continue
            i = i + 1
        line_no = line_no + 1
    return "[" + ",".join(parts) + "]"

def handle_document_highlight(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    pos: unknown = minijson.get_obj(params, "position")
    line: int = minijson.get_int(pos, "line")
    character: int = minijson.get_int(pos, "character")
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    text: str = doc_texts[idx]
    word: str = word_at(text, line, character)
    if word == "" or is_py_keyword(word):
        send_frame(rpc_result(id_text, "null"))
        return
    send_frame(rpc_result(id_text, highlight_ranges(text, word)))

# ---------------- foldingRange：def/class 块、连续注释块、三引号字符串块 ----------------

def add_fold(starts: list[int], ends: list[int], kinds: list[str],
             sl: int, el: int, kind: str) -> None:
    if el <= sl:
        return
    starts.append(sl)
    ends.append(el)
    kinds.append(kind)

def folding_ranges_json(text: str) -> str:
    starts: list[int] = []
    ends: list[int] = []
    kinds: list[str] = []
    lines: list[str] = text.split("\n")
    # 1) def / class 块（缩进驱动；scan_symbols 的 end_line 就是块最后一行）
    syms: list[Symbol] = scan_symbols(text)
    i: int = 0
    while i < len(syms):
        s: Symbol = syms[i]
        if s.kind == 5 or s.kind == 12:
            add_fold(starts, ends, kinds, s.line, s.end_line, "region")
        i = i + 1
    # 2) 连续注释块（>=2 行）
    j: int = 0
    while j < len(lines):
        if lines[j].strip().startswith("#"):
            k: int = j + 1
            while k < len(lines) and lines[k].strip().startswith("#"):
                k = k + 1
            add_fold(starts, ends, kinds, j, k - 1, "comment")
            j = k
        else:
            j = j + 1
    # 3) 跨行三引号字符串块
    triple: str = ""
    open_line: int = -1
    p: int = 0
    while p < len(lines):
        raw: str = lines[p]
        q: int = 0
        if triple != "":
            close: int = raw.find(triple)
            if close < 0:
                p = p + 1
                continue
            add_fold(starts, ends, kinds, open_line, p, "region")
            triple = ""
            q = close + 3
        while q < len(raw):
            c: str = raw[q]
            if c == "#":
                break
            if raw[q:q + 3] == '"""' or raw[q:q + 3] == "'''":
                q3: str = raw[q:q + 3]
                close2: int = raw.find(q3, q + 3)
                if close2 < 0:
                    triple = q3
                    open_line = p
                    break
                q = close2 + 3
                continue
            if c == '"' or c == "'":
                qc: str = c
                r: int = q + 1
                while r < len(raw):
                    if raw[r] == "\\":
                        r = r + 2
                        continue
                    if raw[r] == qc:
                        r = r + 1
                        break
                    r = r + 1
                q = r
                continue
            q = q + 1
        p = p + 1
    # 按 (startLine, endLine) 排序后输出
    order: list[int] = []
    i2: int = 0
    while i2 < len(starts):
        order.append(i2)
        i2 = i2 + 1
    a: int = 0
    while a < len(order):
        b: int = a + 1
        while b < len(order):
            ia: int = order[a]
            ib: int = order[b]
            if starts[ib] < starts[ia] or (starts[ib] == starts[ia] and ends[ib] < ends[ia]):
                order[a] = ib
                order[b] = ia
            b = b + 1
        a = a + 1
    parts: list[str] = []
    i3: int = 0
    while i3 < len(order):
        k3: int = order[i3]
        parts.append('{"startLine":' + str(starts[k3]) + ',"endLine":' + str(ends[k3]) +
                     ',"kind":' + minijson.quote(kinds[k3]) + '}')
        i3 = i3 + 1
    return "[" + ",".join(parts) + "]"

def handle_folding_range(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "[]"))
        return
    send_frame(rpc_result(id_text, folding_ranges_json(doc_texts[idx])))


# ---------------- 各方法处理 ----------------

def init_result() -> str:
    # 能力表：文档同步 + hover + documentSymbol + completion + definition +
    # semanticTokens。semanticTokens 的 legend 顺序必须和 semantic_tokens()
    # 里 TOKEN_* 常量一致（客户端按索引解释）。
    types_json: list[str] = []
    i: int = 0
    while i < len(TOKEN_TYPES):
        types_json.append(minijson.quote(TOKEN_TYPES[i]))
        i = i + 1
    caps: str = ('{"textDocumentSync":{"openClose":true,"change":1},'
                 '"hoverProvider":true,"documentSymbolProvider":true,'
                 '"completionProvider":{"triggerCharacters":["."]},'
                 '"definitionProvider":true,'
                 '"semanticTokensProvider":{"legend":{"tokenTypes":[' +
                 ",".join(types_json) + '],"tokenModifiers":[]},"full":true},'
                 '"signatureHelpProvider":{"triggerCharacters":["(",","]},'
                 '"documentHighlightProvider":true,"foldingRangeProvider":true}')
    return '{"capabilities":' + caps + ',"serverInfo":{"name":"pya-lsp","version":"0.2.0"}}'

def params_of(msg: unknown) -> unknown:
    return minijson.get_obj(msg, "params")

def uri_of(params: unknown) -> str:
    td: unknown = minijson.get_obj(params, "textDocument")
    return minijson.get_str(td, "uri")

def handle_did_open(params: unknown) -> None:
    td: unknown = minijson.get_obj(params, "textDocument")
    uri: str = minijson.get_str(td, "uri")
    text: str = minijson.get_str(td, "text")
    version: int = minijson.get_int(td, "version")
    if uri == "":
        return
    doc_put(uri, text, version)
    publish_diagnostics(uri, text, version)

def apply_did_change(params: unknown) -> str:
    # 把一帧 didChange 的全部 contentChanges 合并进文档表，返回 uri。
    td: unknown = minijson.get_obj(params, "textDocument")
    uri: str = minijson.get_str(td, "uri")
    version: int = minijson.get_int(td, "version")
    if uri == "":
        return ""
    idx: int = doc_index(uri)
    text: str = ""
    if idx >= 0:
        text = doc_texts[idx]
    changes: unknown = minijson.get_arr(params, "contentChanges")
    count: int = minijson.size(changes)
    k: int = 0
    while k < count:
        text = apply_change(text, minijson.at(changes, k))
        k = k + 1
    doc_put(uri, text, version)
    return uri

def handle_did_change(params: unknown, inp: file) -> None:
    uri: str = apply_did_change(params)
    if uri == "":
        return
    # debounce：把紧接着到达的 didChange 合并，只跑最后一次诊断。
    drain_did_changes(inp, uri)

def handle_did_close(params: unknown) -> None:
    uri: str = uri_of(params)
    if uri == "":
        return
    doc_remove(uri)
    clear: str = '{"uri":' + minijson.quote(uri) + ',"diagnostics":[]}'
    send_frame(rpc_notification("textDocument/publishDiagnostics", clear))

def handle_document_symbol(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    syms: list[Symbol] = scan_symbols(doc_texts[idx])
    send_frame(rpc_result(id_text, symbols_json(syms)))

def handle_hover(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    pos: unknown = minijson.get_obj(params, "position")
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


# ---------------- didChange 合并（debounce）+ 待处理消息队列 ----------------
#
# LSP 是单线程读循环，没有 timer 线程。这里用「读下一帧之前先等一小段」
# 实现去抖：didChange 到达后，最多等 debounce 毫秒，看 stdin 上是否紧跟
# 下一个帧；紧跟的 didChange 全部合并（只跑最后一次诊断），遇到请求则把
# 它排进队列、立刻发布诊断再继续主循环（不让窗口拖慢请求）。
# stdin 用 open(0, "rb", 0) 无缓冲读，fd 可读性 == 是否有待处理字节；
# 用 asyncio.wait_readable + wait_for 做带超时的 poll（不消费数据）。
pending_bodies: list[str] = []

def debounce_ms() -> int:
    v: str = env_str("PYA_LSP_DIAG_DEBOUNCE_MS")
    if not is_digit_str(v):
        return 300
    return int(v)

async def _wait_readable_for(ms: int) -> bool:
    try:
        await asyncio.wait_for(asyncio.wait_readable(0), float(ms) / 1000.0)
        return True
    except TimeoutError:
        return False

def stdin_ready(ms: int) -> bool:
    if ms < 1:
        return False
    try:
        return asyncio.run(_wait_readable_for(ms))
    except Exception:
        return False

def next_body(inp: file) -> str:
    if len(pending_bodies) > 0:
        b: str = pending_bodies[0]
        pending_bodies.pop(0)
        return b
    return read_frame(inp)

def parse_body(body: str) -> unknown:
    try:
        return minijson.parse(body)
    except ValueError:
        log("malformed JSON, ignored")
        return minijson.new_null()

def drain_did_changes(inp: file, uri: str) -> None:
    dirty: list[str] = [uri]
    window: int = debounce_ms()
    if window <= 0:
        idx0: int = doc_index(uri)
        if idx0 >= 0:
            publish_diagnostics(uri, doc_texts[idx0], doc_versions[idx0])
        return
    deadline: int = int(time.time() * 1000.0) + window
    while True:
        now: int = int(time.time() * 1000.0)
        remain: int = deadline - now
        if remain <= 0:
            break
        if not stdin_ready(remain):
            break
        body: str = read_frame(inp)
        if body == "":
            pending_bodies.append("")
            break
        msg: unknown = parse_body(body)
        method: str = minijson.get_str(msg, "method")
        if method == "textDocument/didChange":
            p2: unknown = params_of(msg)
            apply_did_change(p2)
            u2: str = uri_of(p2)
            if not list_has(dirty, u2):
                dirty.append(u2)
            # 每来一帧重置窗口：连续输入只保留最后一次诊断。
            deadline = int(time.time() * 1000.0) + window
        else:
            pending_bodies.append(body)
            break
    i: int = 0
    while i < len(dirty):
        u3: str = dirty[i]
        idx: int = doc_index(u3)
        if idx >= 0:
            publish_diagnostics(u3, doc_texts[idx], doc_versions[idx])
        i = i + 1

# ---------------- 主循环 ----------------

def run() -> int:
    shutdown_requested: bool = False
    inp: file = open_stdin()
    while True:
        body: str = next_body(inp)
        if body == "":
            log("stdin closed, exiting")
            return 0
        log("recv body bytes=" + str(len(body)))
        msg: unknown = parse_body(body)
        if minijson.is_null(msg):
            continue
        method: str = minijson.get_str(msg, "method")
        if method == "":
            # 客户端对服务端请求的响应：本服务器不发请求，忽略
            continue
        has_id: bool = minijson.has(msg, "id")
        id_text: str = "null"
        if has_id:
            id_text = minijson.stringify(minijson.get(msg, "id"))
        params: unknown = params_of(msg)
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
            handle_did_change(params, inp)
        elif method == "textDocument/didClose":
            handle_did_close(params)
        elif method == "textDocument/documentSymbol":
            handle_document_symbol(id_text, params)
        elif method == "textDocument/hover":
            handle_hover(id_text, params)
        elif method == "textDocument/completion":
            handle_completion(id_text, params)
        elif method == "textDocument/definition":
            handle_definition(id_text, params)
        elif method == "textDocument/semanticTokens/full":
            handle_semantic_tokens(id_text, params)
        elif method == "textDocument/signatureHelp":
            handle_signature_help(id_text, params)
        elif method == "textDocument/documentHighlight":
            handle_document_highlight(id_text, params)
        elif method == "textDocument/foldingRange":
            handle_folding_range(id_text, params)
        else:
            if has_id:
                send_frame(rpc_error(id_text, -32601, "method not found: " + method))

def main() -> None:
    sys.exit(run())

main()
