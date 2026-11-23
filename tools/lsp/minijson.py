# tools/lsp/minijson.py
# 动态 JSON 的**函数式访问薄层**（供 LSP 服务器与测试共用）。
#
# 历史：这里原来是手写的 JSON parser/stringifier（543 行，JVal 树 + 自己
# 的转义/数字格式化）。0.447 起 runtime 提供了动态 JSON：
#   json.parse(text) -> unknown
#   json.stringify(v) -> str
#   json.get/get_or/has/type/as_dict/as_list/as_str/as_int/as_bool/len/keys
# 于是本文件改成薄封装：**只替换 parse/stringify 两个入口**，其余访问器
# 保持原来的「宽容语义」（取不到/类型不符返回 null、""、0、False、[]），
# 这样 LSP 主程序的既有调用点一行不用改。
#
# 覆盖的入口（与旧 JVal 版本同名同语义）：
#   parse/stringify/quote/byte_len/new_null
#   kind/is_null/is_bool/is_int/is_number/is_str/is_arr/is_obj
#   as_str/as_int/as_float/as_bool
#   has/get/get_str/get_int/get_bool/get_obj/get_arr/at/size/obj_keys
#   obj_set/arr_add
import json


# ---------------- 构造/序列化 ----------------

def new_null() -> unknown:
    return json.null()


def parse(text: str) -> unknown:
    return json.parse(text)


def stringify(v: unknown) -> str:
    return json.stringify(v)


def quote(s: str) -> str:
    # 字符串字面量的 JSON 形式（旧版把非 ASCII 转成 \uXXXX；json.stringify
    # 的默认 ensure_ascii 口径相同，所以回包体保持纯 ASCII）。
    return json.stringify(s)


def byte_len(s: str) -> int:
    # Content-Length 要 UTF-8 字节数；len_bytes 是 O(1) 内置。
    return len_bytes(s)


# ---------------- 类型判定（旧 kind 编码：0 null/1 bool/2 int/3 float/4 str/5 arr/6 obj） ----------------

def kind(v: unknown) -> int:
    t: str = json.type(v)
    if t == "null":
        return 0
    if t == "bool":
        return 1
    if t == "int":
        return 2
    if t == "float":
        return 3
    if t == "str":
        return 4
    if t == "list":
        return 5
    if t == "dict":
        return 6
    return -1


def is_null(v: unknown) -> bool:
    return json.is_null(v)


def is_bool(v: unknown) -> bool:
    return json.type(v) == "bool"


def is_int(v: unknown) -> bool:
    return json.type(v) == "int"


def is_number(v: unknown) -> bool:
    t: str = json.type(v)
    return t == "int" or t == "float"


def is_str(v: unknown) -> bool:
    return json.type(v) == "str"


def is_arr(v: unknown) -> bool:
    return json.type(v) == "list"


def is_obj(v: unknown) -> bool:
    return json.type(v) == "dict"


# ---------------- 取值（宽容语义） ----------------

def as_str(v: unknown) -> str:
    if json.type(v) == "str":
        return json.as_str(v)
    return ""


def as_int(v: unknown) -> int:
    t: str = json.type(v)
    if t == "int":
        return json.as_int(v)
    if t == "float":
        return int(json.as_float(v))
    if t == "bool":
        if json.as_bool(v):
            return 1
        return 0
    return 0


def as_float(v: unknown) -> float:
    t: str = json.type(v)
    if t == "float":
        return json.as_float(v)
    if t == "int":
        return float(json.as_int(v))
    if t == "bool":
        if json.as_bool(v):
            return 1.0
        return 0.0
    return 0.0


def as_bool(v: unknown) -> bool:
    if json.type(v) == "bool":
        return json.as_bool(v)
    return False


def has(v: unknown, key: str) -> bool:
    if json.type(v) != "dict":
        return False
    return json.has(v, key)


def get(v: unknown, key: str) -> unknown:
    if json.type(v) != "dict":
        return json.null()
    return json.get_or(v, key, json.null())


def get_str(v: unknown, key: str) -> str:
    return as_str(get(v, key))


def get_int(v: unknown, key: str) -> int:
    return as_int(get(v, key))


def get_bool(v: unknown, key: str) -> bool:
    return as_bool(get(v, key))


def get_obj(v: unknown, key: str) -> unknown:
    return get(v, key)


def get_arr(v: unknown, key: str) -> unknown:
    return get(v, key)


def at(v: unknown, i: int) -> unknown:
    if json.type(v) != "list":
        return json.null()
    n: int = json.len(v)
    if i < 0 or i >= n:
        return json.null()
    return json.get(v, i)


def size(v: unknown) -> int:
    t: str = json.type(v)
    if t == "list" or t == "dict" or t == "str":
        return json.len(v)
    return 0


def obj_keys(v: unknown) -> list[str]:
    if json.type(v) != "dict":
        return []
    return json.keys(v)


# ---------------- 原地修改（仅用于构建请求/测试数据） ----------------

def obj_set(v: unknown, key: str, val: unknown) -> None:
    if json.type(v) != "dict":
        return
    d: dict[str, unknown] = json.as_dict(v)
    d[key] = val


def arr_add(v: unknown, val: unknown) -> None:
    if json.type(v) != "list":
        return
    xs: list[unknown] = json.as_list(v)
    xs.append(val)
