# tools/lsp/lsp_intel.py
# 代码理解类请求：definition / hover / semanticTokens / signatureHelp / documentHighlight。
import minijson

from lsp_text import find_def_line, find_def_location, is_digit_char, is_ident_char, is_param, is_py_keyword, list_has, offset_at, params_text, read_ident, scan_ident_end, strip_trailing_colon, u16_prefix_len, word_at
from lsp_protocol import location_json, range_json, rpc_result, send_frame, uri_of
from lsp_state import doc_index
import lsp_state
from lsp_doc import class_name_set, func_name_set

# ---------------- 跳转定义：标识符 -> 定义位置 ----------------

def handle_definition(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    pos: unknown = minijson.get_obj(params, "position")
    line: int = minijson.get_int(pos, "line")
    character: int = minijson.get_int(pos, "character")
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    text: str = lsp_state.doc_texts[idx]
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
    send_frame(rpc_result(id_text, semantic_tokens(lsp_state.doc_texts[idx])))

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
    text: str = lsp_state.doc_texts[idx]
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
    text: str = lsp_state.doc_texts[idx]
    word: str = word_at(text, line, character)
    if word == "" or is_py_keyword(word):
        send_frame(rpc_result(id_text, "null"))
        return
    send_frame(rpc_result(id_text, highlight_ranges(text, word)))

def handle_hover(id_text: str, params: unknown) -> None:
    uri: str = uri_of(params)
    pos: unknown = minijson.get_obj(params, "position")
    line: int = minijson.get_int(pos, "line")
    character: int = minijson.get_int(pos, "character")
    idx: int = doc_index(uri)
    if idx < 0:
        send_frame(rpc_result(id_text, "null"))
        return
    text: str = lsp_state.doc_texts[idx]
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
