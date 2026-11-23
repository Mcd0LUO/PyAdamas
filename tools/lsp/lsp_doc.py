# tools/lsp/lsp_doc.py
# 文档模型与文档级请求：Symbol 扫描、补全候选、documentSymbol / foldingRange /
# workspace/symbol / formatting。
#
# 注意：Symbol 类与它的所有字段读写必须在同一个模块里——当前编译器不支持
# 跨模块 class 的字段读/方法调用（见 tools/lsp/LANGUAGE_GAPS.md）。
import minijson

from lsp_text import PY_KEYWORDS, cp_index_at, first_non_space, indent_width, is_ident_char, leading_ws, list_has, read_ident, strip_trailing_colon, truncate, u16_prefix_len
from lsp_protocol import log, range_json, rpc_result, send_frame, uri_of
from lsp_state import collect_py_files, doc_index, path_to_file_uri, read_text_file
import lsp_state

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

# ---------------- 自动补全：文档符号 + import 成员 + Python 关键字 ----------------

# CompletionItemKind（LSP 规范）：1 Text / 2 Method / 3 Function / 5 Field /
# 6 Variable / 7 Class / 9 Module / 14 Keyword。
comp_kind_function: int = 3
comp_kind_class: int = 7
comp_kind_variable: int = 6
comp_kind_module: int = 9
comp_kind_keyword: int = 14

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
                          collect_completions(lsp_state.doc_texts[idx], line, character)))

# ---------------- 跳转定义：标识符 -> 定义位置 ----------------

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
    send_frame(rpc_result(id_text, folding_ranges_json(lsp_state.doc_texts[idx])))

def workspace_symbols_json(query: str) -> str:
    if lsp_state.workspace_root == "":
        return "[]"
    files: list[str] = []
    collect_py_files(lsp_state.workspace_root, 0, files)
    parts: list[str] = []
    fi: int = 0
    while fi < len(files):
        path: str = files[fi]
        text: str = read_text_file(path)
        uri: str = path_to_file_uri(path)
        syms: list[Symbol] = scan_symbols(text)
        si: int = 0
        while si < len(syms):
            s: Symbol = syms[si]
            if query == "" or s.name.find(query) >= 0:
                parts.append('{"name":' + minijson.quote(s.name) +
                             ',"kind":' + str(s.kind) +
                             ',"location":{"uri":' + minijson.quote(uri) +
                             ',"range":' + range_json(s.line, s.col, s.line,
                                                      s.col + len(s.name)) + '}}')
            si = si + 1
        fi = fi + 1
    log("workspace/symbol -> " + str(len(parts)) + " symbol(s) from " +
        str(len(files)) + " file(s)")
    return "[" + ",".join(parts) + "]"

def handle_workspace_symbol(id_text: str, params: unknown) -> None:
    query: str = minijson.get_str(params, "query")
    send_frame(rpc_result(id_text, workspace_symbols_json(query)))

# ---------------- formatting：保守缩进归一 + 行尾空白 ----------------
#
# 只改两件事：行首缩进（按块层级归一到 4 空格）与行尾空白。
# 绝不重排表达式、合并/拆分代码行；括号续行与三引号字符串内部原样保留。

class LineScan:
    delta: int
    opens: bool
    triple: str
    code_end: int

    def __init__(self) -> None:
        self.delta = 0
        self.opens = False
        self.triple = ""
        self.code_end = -1

def scan_code_line(raw: str) -> LineScan:
    st: LineScan = LineScan()
    i: int = 0
    quote: str = ""
    while i < len(raw):
        c: str = raw[i]
        if quote != "":
            if c == "\\":
                i = i + 2
                continue
            if c == quote:
                quote = ""
                st.code_end = i
            i = i + 1
            continue
        if c == "#":
            break
        if c == '"' or c == "'":
            if raw[i:i + 3] == c + c + c:
                j: int = raw.find(c + c + c, i + 3)
                if j < 0:
                    st.triple = c + c + c
                    st.code_end = i + 2
                    break
                st.code_end = j + 2
                i = j + 3
                continue
            quote = c
            st.code_end = i
            i = i + 1
            continue
        if c == "(" or c == "[" or c == "{":
            st.delta = st.delta + 1
        elif c == ")" or c == "]" or c == "}":
            st.delta = st.delta - 1
        if c != " " and c != "\t":
            st.code_end = i
        i = i + 1
    st.opens = st.code_end >= 0 and raw[st.code_end] == ":"
    return st

def fmt_indent_width(raw: str) -> int:
    w: int = 0
    i: int = 0
    while i < len(raw):
        c: str = raw[i]
        if c == " ":
            w = w + 1
        elif c == "\t":
            w = w + 4
        else:
            break
        i = i + 1
    return w

def is_dedent_keyword(s: str) -> bool:
    if s == "else:" or s.startswith("else: "):
        return True
    if s.startswith("elif "):
        return True
    if s == "except:" or s.startswith("except ") or s.startswith("except("):
        return True
    if s == "finally:" or s.startswith("finally: "):
        return True
    return False

def line_edit_json(line_no: int, old: str, new: str) -> str:
    end: int = u16_prefix_len(old, len(old))
    return ('{"range":' + range_json(line_no, 0, line_no, end) +
            ',"newText":' + minijson.quote(new) + '}')

def compute_format_edits(text: str) -> list[str]:
    lines: list[str] = text.split("\n")
    edits: list[str] = []
    widths: list[int] = [0]
    levels: list[int] = [0]
    bracket: int = 0
    prev_opens: bool = False
    triple: str = ""
    ln: int = 0
    while ln < len(lines):
        raw: str = lines[ln]
        if triple != "":
            if raw.find(triple) >= 0:
                triple = ""
            ln = ln + 1
            continue
        st: LineScan = scan_code_line(raw)
        if st.triple != "":
            triple = st.triple
        content: str = raw[leading_ws(raw):]
        if content.strip() == "":
            if raw != "" and raw != "\r":
                blank: str = ""
                if raw.endswith("\r"):
                    blank = "\r"
                edits.append(line_edit_json(ln, raw, blank))
            ln = ln + 1
            continue
        cur_w: int = fmt_indent_width(raw)
        continuation: bool = bracket > 0
        if not continuation:
            if is_dedent_keyword(content.strip()) and len(widths) > 1:
                widths.pop()
                levels.pop()
            while len(widths) > 1 and cur_w < widths[len(widths) - 1]:
                widths.pop()
                levels.pop()
            if cur_w > widths[len(widths) - 1]:
                if prev_opens:
                    widths.append(cur_w)
                    levels.append(levels[len(levels) - 1] + 1)
                else:
                    continuation = True
        if continuation:
            bracket = bracket + st.delta
            prev_opens = st.opens
            ln = ln + 1
            continue
        lvl: int = levels[len(levels) - 1]
        desired: str = " " * (4 * lvl)
        if st.triple != "":
            new_line: str = desired + content
        else:
            cr: str = ""
            body: str = content
            if body.endswith("\r"):
                cr = "\r"
                body = body[:len(body) - 1]
            new_line = desired + body.rstrip() + cr
        if new_line != raw:
            edits.append(line_edit_json(ln, raw, new_line))
        bracket = bracket + st.delta
        prev_opens = st.opens
        ln = ln + 1
    return edits

def handle_formatting(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "[]"))
        return
    edits: list[str] = compute_format_edits(lsp_state.doc_texts[idx])
    log("formatting -> " + str(len(edits)) + " edit(s)")
    send_frame(rpc_result(id_text, "[" + ",".join(edits) + "]"))

def handle_document_symbol(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    syms: list[Symbol] = scan_symbols(lsp_state.doc_texts[idx])
    send_frame(rpc_result(id_text, symbols_json(syms)))
