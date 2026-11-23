# tools/lsp/lsp_refs.py
# references / rename：文档内符号查找与重命名（词法扫描 + 粗粒度作用域分区）。
import minijson

from lsp_text import cp_index_at, is_ident_char, is_py_keyword, leading_ws, list_has, params_text, read_ident, scan_ident_end, strip_trailing_colon, u16_prefix_len
from lsp_protocol import location_json, log, range_json, rpc_error, rpc_result, send_frame, uri_of
from lsp_state import doc_index
import lsp_state

# ---------------- references / rename：文档内符号查找与重命名 ----------------
#
# 只搜当前文档（跨文件需要 workspace 索引，本轮不做）。词法扫描会跳过注释和
# 字符串（含三引号跨行字符串），所以中文注释/字符串里出现的同名文本不会被
# 当成引用；标识符本身仍是 ASCII（is_ident_char 的口径）。
#
# 作用域粗分区：每个 def/class 是一个块（按缩进算行范围）。同名局部变量按
# 「所在 def 块」区分：形参 / 块内赋值 / for-with-except 绑定都算该块的局部
# 绑定；嵌套 def 里重新绑定的同名符号不会算进外层。全局变量/函数/类则收集
# 所有未被局部绑定遮蔽的出现位置。

class WordTok:
    line: int
    start: int   # 行内码点下标
    end: int
    word: str

    def __init__(self) -> None:
        self.line = 0
        self.start = 0
        self.end = 0
        self.word = ""

class Block:
    kind: int      # 0 顶层 / 1 def / 2 class
    name: str
    indent: int
    first: int     # 块首行（def/class 行）
    last: int      # 块尾行
    header: int
    params: list[str]

    def __init__(self) -> None:
        self.kind = 0
        self.name = ""
        self.indent = -1
        self.first = 0
        self.last = 0
        self.header = 0
        self.params = []

def param_name_list(sig: str) -> list[str]:
    out: list[str] = []
    params: str = params_text(sig)
    if params.strip() == "":
        return out
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
        if nm != "":
            out.append(nm)
        i = i + 1
    return out

def scan_blocks(text: str) -> list[Block]:
    lines: list[str] = text.split("\n")
    blocks: list[Block] = []
    top: Block = Block()
    top.kind = 0
    top.name = ""
    top.indent = -1
    top.first = 0
    top.last = len(lines) - 1
    top.header = 0
    blocks.append(top)
    i: int = 0
    while i < len(lines):
        raw: str = lines[i]
        ind: int = leading_ws(raw)
        stripped: str = raw[ind:]
        kind: int = 0
        off: int = 0
        if stripped.startswith("async def "):
            kind = 1
            off = len("async def ")
        elif stripped.startswith("def "):
            kind = 1
            off = len("def ")
        elif stripped.startswith("class "):
            kind = 2
            off = len("class ")
        if kind == 0:
            i = i + 1
            continue
        name: str = read_ident(stripped, off)
        if name == "":
            i = i + 1
            continue
        b: Block = Block()
        b.kind = kind
        b.name = name
        b.indent = ind
        b.first = i
        b.header = i
        b.params = param_name_list(strip_trailing_colon(stripped))
        last: int = i
        j: int = i + 1
        while j < len(lines):
            s2: str = lines[j].strip()
            if s2 == "" or s2.startswith("#"):
                j = j + 1
                continue
            if leading_ws(lines[j]) <= ind:
                break
            last = j
            j = j + 1
        b.last = last
        blocks.append(b)
        i = i + 1
    return blocks

def innermost_def_block(blocks: list[Block], line: int) -> int:
    # 返回包含该行的最内层 def 块（blocks 下标）；没有则 -1。
    best: int = -1
    best_span: int = 1000000
    i: int = 1
    while i < len(blocks):
        b: Block = blocks[i]
        if b.kind == 1 and b.first <= line and line <= b.last:
            span: int = b.last - b.first
            if span < best_span:
                best_span = span
                best = i
        i = i + 1
    return best

def line_binds(stripped: str, word: str) -> bool:
    # 这一行是否把 word 绑定为局部名（赋值/标注赋值/for/with as/except as）。
    if stripped.startswith("for "):
        rest: str = stripped[len("for "):]
        ipos: int = rest.find(" in ")
        if ipos > 0:
            targets: str = rest[:ipos]
            parts: list[str] = targets.split(",")
            k: int = 0
            while k < len(parts):
                if parts[k].strip() == word:
                    return True
                k = k + 1
            return False
    if stripped.startswith("with ") or stripped.startswith("except "):
        ap: int = stripped.find(" as ")
        if ap >= 0:
            after: str = stripped[ap + len(" as "):]
            nm: str = read_ident(after, 0)
            if nm == word:
                return True
        return False
    if stripped.startswith(word):
        rest2: str = stripped[len(word):]
        if rest2 == "":
            return False
        c0: str = rest2[0]
        if c0 == ":" or c0 == "=" or c0 == ",":
            return True
    return False

def block_local_decl_lines(blocks: list[Block], bi: int, lines: list[str],
                           word: str) -> list[int]:
    out: list[int] = []
    if bi < 0:
        return out
    b: Block = blocks[bi]
    k: int = b.first
    while k <= b.last and k < len(lines):
        stripped: str = lines[k].strip()
        if stripped != "" and not stripped.startswith("#") and line_binds(stripped, word):
            out.append(k)
        k = k + 1
    return out

def scan_words(text: str) -> list[WordTok]:
    # 词法扫描：跳过注释与字符串（含三引号跨行），只收集 ASCII 标识符。
    out: list[WordTok] = []
    lines: list[str] = text.split("\n")
    triple: str = ""
    ln: int = 0
    while ln < len(lines):
        raw: str = lines[ln]
        i: int = 0
        if triple != "":
            close: int = raw.find(triple)
            if close < 0:
                ln = ln + 1
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
                t: WordTok = WordTok()
                t.line = ln
                t.start = i
                t.end = j2
                t.word = raw[i:j2]
                out.append(t)
                i = j2
                continue
            i = i + 1
        ln = ln + 1
    return out

def word_span_at(text: str, line: int, character: int) -> WordTok:
    t: WordTok = WordTok()
    lines: list[str] = text.split("\n")
    if line < 0 or line >= len(lines):
        return t
    raw: str = lines[line]
    idx: int = cp_index_at(raw, character)
    start: int = idx
    while start > 0 and is_ident_char(raw[start - 1]):
        start = start - 1
    end: int = idx
    while end < len(raw) and is_ident_char(raw[end]):
        end = end + 1
    if end > start:
        t.line = line
        t.start = start
        t.end = end
        t.word = raw[start:end]
    return t

def first_token_on_line(toks: list[WordTok], line: int, word: str) -> WordTok:
    best: WordTok = WordTok()
    i: int = 0
    while i < len(toks):
        t: WordTok = toks[i]
        if t.line == line and t.word == word:
            if best.word == "" or t.start < best.start:
                best = t
        i = i + 1
    return best

def declaration_tokens(text: str, toks: list[WordTok], blocks: list[Block],
                       word: str, kind: str, scope: int) -> list[WordTok]:
    # 返回 word 在当前解析上下文里的声明 token（def/class 名、形参、赋值目标）。
    out: list[WordTok] = []
    lines: list[str] = text.split("\n")
    if kind == "global":
        i: int = 1
        while i < len(blocks):
            b: Block = blocks[i]
            if b.name == word:
                t: WordTok = first_token_on_line(toks, b.header, word)
                if t.word != "":
                    out.append(t)
            i = i + 1
        # 顶层赋值
        k: int = 0
        while k < len(lines):
            if innermost_def_block(blocks, k) < 0:
                stripped: str = lines[k].strip()
                if stripped != "" and not stripped.startswith("#") and line_binds(stripped, word):
                    t2: WordTok = first_token_on_line(toks, k, word)
                    if t2.word != "":
                        out.append(t2)
            k = k + 1
        return out
    # 局部（形参 / 函数体内绑定）
    if scope < 0:
        return out
    b2: Block = blocks[scope]
    k2: int = 0
    while k2 < len(b2.params):
        if b2.params[k2] == word:
            t3: WordTok = first_token_on_line(toks, b2.header, word)
            if t3.word != "":
                out.append(t3)
            break
        k2 = k2 + 1
    dl: list[int] = block_local_decl_lines(blocks, scope, lines, word)
    k3: int = 0
    while k3 < len(dl):
        t4: WordTok = first_token_on_line(toks, dl[k3], word)
        if t4.word != "":
            out.append(t4)
        k3 = k3 + 1
    return out

def same_tok(a: WordTok, b: WordTok) -> bool:
    return a.line == b.line and a.start == b.start

def collect_ref_tokens(text: str, line: int, character: int,
                       include_decl: bool) -> list[WordTok]:
    toks: list[WordTok] = scan_words(text)
    lines: list[str] = text.split("\n")
    cur: WordTok = word_span_at(text, line, character)
    if cur.word == "":
        return []
    word: str = cur.word
    if is_py_keyword(word):
        return []
    blocks: list[Block] = scan_blocks(text)
    cur_block: int = innermost_def_block(blocks, line)
    kind: str = ""
    scope: int = -1
    if cur_block >= 0:
        b: Block = blocks[cur_block]
        if list_has(b.params, word):
            kind = "local"
            scope = cur_block
        elif len(block_local_decl_lines(blocks, cur_block, lines, word)) > 0:
            kind = "local"
            scope = cur_block
    if kind == "":
        # 全局：有 def/class 名，或顶层赋值，才算可解析
        if len(declaration_tokens(text, toks, blocks, word, "global", -1)) > 0:
            kind = "global"
    if kind == "":
        return []
    decls: list[WordTok] = declaration_tokens(text, toks, blocks, word, kind, scope)
    refs: list[WordTok] = []
    i: int = 0
    while i < len(toks):
        t: WordTok = toks[i]
        if t.word == word:
            ok: bool = False
            if kind == "local":
                if blocks[scope].first <= t.line and t.line <= blocks[scope].last:
                    nb: int = innermost_def_block(blocks, t.line)
                    if nb < 0 or nb == scope:
                        ok = True
                    else:
                        # 嵌套块里重新绑定的同名符号不算外层引用
                        hid: bool = list_has(blocks[nb].params, word)
                        if not hid:
                            hid = len(block_local_decl_lines(blocks, nb,
                                                             lines, word)) > 0
                        ok = not hid
            else:
                nb2: int = innermost_def_block(blocks, t.line)
                if nb2 < 0:
                    ok = True
                else:
                    shadow: bool = list_has(blocks[nb2].params, word)
                    if not shadow:
                        shadow = len(block_local_decl_lines(blocks, nb2,
                                                            lines, word)) > 0
                    ok = not shadow
            if ok:
                refs.append(t)
        i = i + 1
    if include_decl:
        return refs
    # 去掉声明位置
    out: list[WordTok] = []
    i = 0
    while i < len(refs):
        t2: WordTok = refs[i]
        is_decl: bool = False
        k: int = 0
        while k < len(decls):
            if same_tok(t2, decls[k]):
                is_decl = True
                break
            k = k + 1
        if not is_decl:
            out.append(t2)
        i = i + 1
    return out

def line_utf16_start(lines: list[str], tok: WordTok) -> int:
    if tok.line < 0 or tok.line >= len(lines):
        return 0
    return u16_prefix_len(lines[tok.line], tok.start)

def tok_utf16_len(lines: list[str], tok: WordTok) -> int:
    if tok.line < 0 or tok.line >= len(lines):
        return 0
    return (u16_prefix_len(lines[tok.line], tok.end) -
            u16_prefix_len(lines[tok.line], tok.start))

def handle_references(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    pos: unknown = minijson.get_obj(params, "position")
    line: int = minijson.get_int(pos, "line")
    character: int = minijson.get_int(pos, "character")
    ctx: unknown = minijson.get_obj(params, "context")
    include_decl: bool = minijson.get_bool(ctx, "includeDeclaration")
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "[]"))
        return
    text: str = lsp_state.doc_texts[idx]
    lines: list[str] = text.split("\n")
    toks: list[WordTok] = collect_ref_tokens(text, line, character, include_decl)
    parts: list[str] = []
    i: int = 0
    while i < len(toks):
        t: WordTok = toks[i]
        parts.append(location_json(uri, t.line, line_utf16_start(lines, t),
                                   tok_utf16_len(lines, t)))
        i = i + 1
    log("references -> " + str(len(parts)) + " location(s)")
    send_frame(rpc_result(id_text, "[" + ",".join(parts) + "]"))

def valid_ident_name(s: str) -> bool:
    if s == "" or is_py_keyword(s):
        return False
    if not is_ident_char(s[0]):
        return False
    if s[0] >= "0" and s[0] <= "9":
        return False
    i: int = 1
    while i < len(s):
        if not is_ident_char(s[i]):
            return False
        i = i + 1
    return True

def handle_rename(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    pos: unknown = minijson.get_obj(params, "position")
    line: int = minijson.get_int(pos, "line")
    character: int = minijson.get_int(pos, "character")
    new_name: str = minijson.get_str(params, "newName")
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    text: str = lsp_state.doc_texts[idx]
    if not valid_ident_name(new_name):
        send_frame(rpc_error(id_text, -32602,
                             "newName is not a valid identifier: " + new_name))
        return
    toks: list[WordTok] = collect_ref_tokens(text, line, character, True)
    if len(toks) == 0:
        send_frame(rpc_result(id_text, "null"))
        return
    lines: list[str] = text.split("\n")
    edits: list[str] = []
    i: int = 0
    while i < len(toks):
        t: WordTok = toks[i]
        st: int = line_utf16_start(lines, t)
        ln: int = tok_utf16_len(lines, t)
        edit: str = ('{"range":' + range_json(t.line, st, t.line, st + ln) +
                     ',"newText":' + minijson.quote(new_name) + '}')
        edits.append(edit)
        i = i + 1
    body: str = '{"changes":{' + minijson.quote(uri) + ':[' + ",".join(edits) + ']}}'
    log("rename -> " + str(len(edits)) + " edit(s)")
    send_frame(rpc_result(id_text, body))
