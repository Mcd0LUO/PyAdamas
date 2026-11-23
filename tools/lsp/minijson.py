# tools/lsp/minijson.py
# 手写的 JSON 解析 / 序列化（LSP 所需子集：对象、数组、字符串、整数、浮点、bool、null）。
#
# 为什么不用内置 json：内置 json 是「按静态类型 dumps / 用 sample 定型 loads」，
# 拿不到 LSP 需要的动态树。等 lsp-lang 落地动态 JSON（json.parse -> unknown）后，
# 可以只替换本文件实现，保持下列函数式 API 不变。
#
# 注意 PyAdamas 的跨模块限制：JVal 只能作为不透明句柄在模块间传递，
# 所有字段访问必须留在本模块内，因此这里导出 get/at/kind 等函数而不是属性。

KIND_NULL: int = 0
KIND_BOOL: int = 1
KIND_INT: int = 2
KIND_FLOAT: int = 3
KIND_STR: int = 4
KIND_ARR: int = 5
KIND_OBJ: int = 6


class JVal:
    kind: int
    b: bool
    i: int
    f: float
    s: str
    arr: list["JVal"]
    obj: dict[str, "JVal"]
    keys: list[str]

    def __init__(self) -> None:
        self.kind = 0
        self.b = False
        self.i = 0
        self.f = 0.0
        self.s = ""
        self.arr = []
        self.obj = {}
        self.keys = []


def new_null() -> JVal:
    return JVal()


def new_bool(b: bool) -> JVal:
    v: JVal = JVal()
    v.kind = 1
    v.b = b
    return v


def new_int(n: int) -> JVal:
    v: JVal = JVal()
    v.kind = 2
    v.i = n
    return v


def new_float(x: float) -> JVal:
    v: JVal = JVal()
    v.kind = 3
    v.f = x
    return v


def new_str(s: str) -> JVal:
    v: JVal = JVal()
    v.kind = 4
    v.s = s
    return v


def new_arr() -> JVal:
    v: JVal = JVal()
    v.kind = 5
    return v


def new_obj() -> JVal:
    v: JVal = JVal()
    v.kind = 6
    return v


def hex4(text: str, pos: int) -> int:
    # 读 4 个十六进制数字，失败返回 -1
    if pos + 4 > len(text):
        return -1
    out: int = 0
    k: int = 0
    while k < 4:
        o: int = ord(text[pos + k])
        d: int = -1
        if o >= 48 and o <= 57:
            d = o - 48
        elif o >= 65 and o <= 70:
            d = o - 55
        elif o >= 97 and o <= 102:
            d = o - 87
        if d < 0:
            return -1
        out = out * 16 + d
        k = k + 1
    return out


class Parser:
    text: str
    pos: int
    n: int

    def __init__(self, text: str) -> None:
        self.text = text
        self.pos = 0
        self.n = len(text)

    def peek(self) -> str:
        if self.pos >= self.n:
            return ""
        return self.text[self.pos]

    def skip_ws(self) -> None:
        while self.pos < self.n:
            c: str = self.text[self.pos]
            if c == " " or c == "\t" or c == "\n" or c == "\r":
                self.pos = self.pos + 1
            else:
                return

    def expect_word(self, w: str) -> None:
        if self.text[self.pos:self.pos + len(w)] != w:
            raise ValueError("invalid JSON literal at offset " + str(self.pos))
        self.pos = self.pos + len(w)

    def parse_value(self) -> JVal:
        self.skip_ws()
        if self.pos >= self.n:
            raise ValueError("unexpected end of JSON input")
        c: str = self.text[self.pos]
        if c == "{":
            return self.parse_object()
        if c == "[":
            return self.parse_array()
        if c == '"':
            return new_str(self.parse_string())
        if c == "t":
            self.expect_word("true")
            return new_bool(True)
        if c == "f":
            self.expect_word("false")
            return new_bool(False)
        if c == "n":
            self.expect_word("null")
            return new_null()
        return self.parse_number()

    def parse_number(self) -> JVal:
        start: int = self.pos
        is_float: bool = False
        while self.pos < self.n:
            c: str = self.text[self.pos]
            o: int = ord(c)
            if o >= 48 and o <= 57:
                self.pos = self.pos + 1
            elif c == "-" or c == "+":
                self.pos = self.pos + 1
            elif c == "." or c == "e" or c == "E":
                is_float = True
                self.pos = self.pos + 1
            else:
                break
        raw: str = self.text[start:self.pos]
        if raw == "":
            raise ValueError("invalid JSON value at offset " + str(start))
        if is_float:
            return new_float(float(raw))
        return new_int(int(raw))

    def parse_string(self) -> str:
        # 进入时 self.text[self.pos] == '"'
        self.pos = self.pos + 1
        parts: list[str] = []
        start: int = self.pos
        while self.pos < self.n:
            c: str = self.text[self.pos]
            if c == '"':
                parts.append(self.text[start:self.pos])
                self.pos = self.pos + 1
                if len(parts) == 1:
                    return parts[0]
                return "".join(parts)
            if c == "\\":
                parts.append(self.text[start:self.pos])
                self.pos = self.pos + 1
                if self.pos >= self.n:
                    raise ValueError("unterminated string escape")
                e: str = self.text[self.pos]
                if e == '"':
                    parts.append('"')
                    self.pos = self.pos + 1
                elif e == "\\":
                    parts.append("\\")
                    self.pos = self.pos + 1
                elif e == "/":
                    parts.append("/")
                    self.pos = self.pos + 1
                elif e == "b":
                    parts.append("\b")
                    self.pos = self.pos + 1
                elif e == "f":
                    parts.append("\f")
                    self.pos = self.pos + 1
                elif e == "n":
                    parts.append("\n")
                    self.pos = self.pos + 1
                elif e == "r":
                    parts.append("\r")
                    self.pos = self.pos + 1
                elif e == "t":
                    parts.append("\t")
                    self.pos = self.pos + 1
                elif e == "u":
                    self.pos = self.pos + 1
                    cp: int = hex4(self.text, self.pos)
                    if cp < 0:
                        raise ValueError("bad \\u escape")
                    self.pos = self.pos + 4
                    if cp >= 0xD800 and cp <= 0xDBFF:
                        # 高代理项：尝试拼下一段 \uXXXX 低代理项
                        if self.pos + 6 <= self.n and self.text[self.pos] == "\\" and self.text[self.pos + 1] == "u":
                            lo: int = hex4(self.text, self.pos + 2)
                            if lo >= 0xDC00 and lo <= 0xDFFF:
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                                self.pos = self.pos + 6
                            else:
                                cp = 0xFFFD
                        else:
                            cp = 0xFFFD
                    elif cp >= 0xDC00 and cp <= 0xDFFF:
                        cp = 0xFFFD
                    parts.append(chr(cp))
                else:
                    raise ValueError("bad escape char")
                start = self.pos
            else:
                self.pos = self.pos + 1
        raise ValueError("unterminated JSON string")

    def parse_object(self) -> JVal:
        v: JVal = new_obj()
        self.pos = self.pos + 1
        self.skip_ws()
        if self.peek() == "}":
            self.pos = self.pos + 1
            return v
        while True:
            self.skip_ws()
            if self.peek() != '"':
                raise ValueError("expected object key at offset " + str(self.pos))
            k: str = self.parse_string()
            self.skip_ws()
            if self.peek() != ":":
                raise ValueError("expected ':' at offset " + str(self.pos))
            self.pos = self.pos + 1
            val: JVal = self.parse_value()
            v.obj[k] = val
            v.keys.append(k)
            self.skip_ws()
            c: str = self.peek()
            if c == ",":
                self.pos = self.pos + 1
            elif c == "}":
                self.pos = self.pos + 1
                return v
            else:
                raise ValueError("expected ',' or '}' at offset " + str(self.pos))

    def parse_array(self) -> JVal:
        v: JVal = new_arr()
        self.pos = self.pos + 1
        self.skip_ws()
        if self.peek() == "]":
            self.pos = self.pos + 1
            return v
        while True:
            val: JVal = self.parse_value()
            v.arr.append(val)
            self.skip_ws()
            c: str = self.peek()
            if c == ",":
                self.pos = self.pos + 1
            elif c == "]":
                self.pos = self.pos + 1
                return v
            else:
                raise ValueError("expected ',' or ']' at offset " + str(self.pos))


def parse(text: str) -> JVal:
    p: Parser = Parser(text)
    v: JVal = p.parse_value()
    p.skip_ws()
    if p.pos != p.n:
        raise ValueError("trailing garbage after JSON value")
    return v


# ---- 访问器（跨模块用函数，不要直接碰 JVal 字段） ----

def kind(v: JVal) -> int:
    return v.kind


def is_null(v: JVal) -> bool:
    return v.kind == 0


def is_bool(v: JVal) -> bool:
    return v.kind == 1


def is_int(v: JVal) -> bool:
    return v.kind == 2


def is_number(v: JVal) -> bool:
    return v.kind == 2 or v.kind == 3


def is_str(v: JVal) -> bool:
    return v.kind == 4


def is_arr(v: JVal) -> bool:
    return v.kind == 5


def is_obj(v: JVal) -> bool:
    return v.kind == 6


def as_str(v: JVal) -> str:
    if v.kind == 4:
        return v.s
    return ""


def as_int(v: JVal) -> int:
    if v.kind == 2:
        return v.i
    if v.kind == 3:
        return int(v.f)
    return 0


def as_float(v: JVal) -> float:
    if v.kind == 3:
        return v.f
    if v.kind == 2:
        return float(v.i)
    return 0.0


def as_bool(v: JVal) -> bool:
    if v.kind == 1:
        return v.b
    return False


def has(v: JVal, key: str) -> bool:
    if v.kind != 6:
        return False
    return key in v.obj


def get(v: JVal, key: str) -> JVal:
    if v.kind != 6:
        return new_null()
    if not (key in v.obj):
        return new_null()
    return v.obj[key]


def get_str(v: JVal, key: str) -> str:
    return as_str(get(v, key))


def get_int(v: JVal, key: str) -> int:
    return as_int(get(v, key))


def get_bool(v: JVal, key: str) -> bool:
    return as_bool(get(v, key))


def get_obj(v: JVal, key: str) -> JVal:
    return get(v, key)


def get_arr(v: JVal, key: str) -> JVal:
    return get(v, key)


def at(v: JVal, i: int) -> JVal:
    if v.kind != 5:
        return new_null()
    if i < 0 or i >= len(v.arr):
        return new_null()
    return v.arr[i]


def size(v: JVal) -> int:
    if v.kind == 5:
        return len(v.arr)
    if v.kind == 6:
        return len(v.keys)
    if v.kind == 4:
        return len(v.s)
    return 0


def obj_keys(v: JVal) -> list[str]:
    if v.kind != 6:
        return []
    return v.keys


def obj_set(v: JVal, key: str, val: JVal) -> None:
    if v.kind != 6:
        return
    if not (key in v.obj):
        v.keys.append(key)
    v.obj[key] = val


def arr_add(v: JVal, val: JVal) -> None:
    if v.kind == 5:
        v.arr.append(val)


# ---- 序列化（输出全部 ASCII，\u 转义非 ASCII，方便按字节计长度） ----

def hex4_out(n: int) -> str:
    digits: str = "0123456789abcdef"
    return digits[(n >> 12) & 15] + digits[(n >> 8) & 15] + digits[(n >> 4) & 15] + digits[n & 15]


def quote(s: str) -> str:
    out: list[str] = ['"']
    i: int = 0
    n: int = len(s)
    while i < n:
        c: str = s[i]
        o: int = ord(c)
        if c == '"':
            out.append("\\\"")
        elif c == "\\":
            out.append("\\\\")
        elif c == "\n":
            out.append("\\n")
        elif c == "\r":
            out.append("\\r")
        elif c == "\t":
            out.append("\\t")
        elif c == "\b":
            out.append("\\b")
        elif c == "\f":
            out.append("\\f")
        elif o < 0x20:
            out.append("\\u" + hex4_out(o))
        elif o < 0x7F:
            out.append(c)
        elif o <= 0xFFFF:
            out.append("\\u" + hex4_out(o))
        else:
            cp: int = o - 0x10000
            hi: int = 0xD800 + (cp >> 10)
            lo: int = 0xDC00 + (cp & 0x3FF)
            out.append("\\u" + hex4_out(hi) + "\\u" + hex4_out(lo))
        i = i + 1
    out.append('"')
    return "".join(out)


def number_text(v: JVal) -> str:
    if v.kind == 2:
        return str(v.i)
    if v.kind == 3:
        return str(v.f)
    return "null"


def stringify(v: JVal) -> str:
    if v.kind == 0:
        return "null"
    if v.kind == 1:
        if v.b:
            return "true"
        return "false"
    if v.kind == 2 or v.kind == 3:
        return number_text(v)
    if v.kind == 4:
        return quote(v.s)
    if v.kind == 5:
        out: list[str] = ["["]
        i: int = 0
        while i < len(v.arr):
            if i > 0:
                out.append(",")
            out.append(stringify(v.arr[i]))
            i = i + 1
        out.append("]")
        return "".join(out)
    out2: list[str] = ["{"]
    i2: int = 0
    while i2 < len(v.keys):
        if i2 > 0:
            out2.append(",")
        out2.append(quote(v.keys[i2]))
        out2.append(":")
        out2.append(stringify(v.obj[v.keys[i2]]))
        i2 = i2 + 1
    out2.append("}")
    return "".join(out2)


def byte_len(s: str) -> int:
    # UTF-8 字节数（Content-Length 要的是字节）
    total: int = 0
    i: int = 0
    n: int = len(s)
    while i < n:
        o: int = ord(s[i])
        if o < 0x80:
            total = total + 1
        elif o < 0x800:
            total = total + 2
        elif o < 0x10000:
            total = total + 3
        else:
            total = total + 4
        i = i + 1
    return total
