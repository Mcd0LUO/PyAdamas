# 232_json_hard.py —— json 健壮化回归（0.314）：对拍 CPython json 语义。
# 键强制转换 / ensure_ascii / separators / allow_nan / indent=0 /
# loads 严格数字文法与 CPython 风格错误消息。expect 段钉自 CPython 3.14。
import json

# ── dict 键强制转换（CPython：int→str、float→floatstr、bool→true/false、None→null）──
print(json.dumps({1: "a", 2: "b"}))
print(json.dumps({10: 1, 2: 2}, sort_keys=True))
print(json.dumps({-5: 1, 3: 2, 0: 0}, sort_keys=True))
print(json.dumps({1.5: "x", 2.0: "y"}))
print(json.dumps({2.5: 1, 1.5: 2}, sort_keys=True))
print(json.dumps({True: 1, False: 0}))
print(json.dumps({True: 1, False: 2}, sort_keys=True))
print(json.dumps({None: 1}))
print(json.dumps({9223372036854775807: 1, (-9223372036854775807 - 1): 2}))
print(json.dumps({1: {2: "x"}}))
print(json.dumps({1: [1, 2], 3: [4]}))
print(json.dumps({1: [1, 2]}, indent=2))
print(json.dumps({0.1: 1, 1e300: 2, 5e-324: 3}))
print(json.dumps({-0.0: 1}))
print(json.dumps({float("nan"): 1, float("inf"): 2, float("-inf"): 3}))

# ── ensure_ascii ──
print(json.dumps("中文"))
print(json.dumps("中文", ensure_ascii=False))
print(json.dumps("a\x7fb"))
print(json.dumps("a\x7fb", ensure_ascii=False))
print(json.dumps("\x00\x1f\x0b"))
print(json.dumps("😀"))
print(json.dumps("😀", ensure_ascii=False))
print(json.dumps({"键": "值"}, ensure_ascii=False))
ea = False
print(json.dumps("变量开关", ensure_ascii=ea))

# ── indent：None=紧凑，0/负数=换行不缩进 ──
print(json.dumps({"a": 1}, indent=0))
print(json.dumps({"a": 1}, indent=-1))
print(json.dumps([1, 2], indent=0))
print(json.dumps({"a": 1}, indent=None))
d: dict[str, int] = {}
print(json.dumps(d, indent=2))
xs: list[int] = []
print(json.dumps(xs, indent=2))
print(json.dumps({"a": [{"b": 2}]}, indent=4))

# ── separators ──
print(json.dumps([1, 2], separators=(",", ":")))
print(json.dumps({"a": 1, "b": 2}, separators=(",", ":")))
print(json.dumps([1, 2], indent=2, separators=(", ", ": ")))
print(json.dumps({"a": 1}, separators=(" ; ", " = ")))
print(json.dumps([1, 2], separators=("", ":")))
sep = (",", ":")
print(json.dumps({"a": [1, 2]}, separators=sep))

# ── allow_nan ──
print(json.dumps(float("nan")), json.dumps(float("inf")), json.dumps(float("-inf")))
print(json.dumps([1.0, float("nan"), float("inf")]))
try:
    json.dumps(float("nan"), allow_nan=False)
except ValueError as e:
    print("ValueError:", str(e))
try:
    json.dumps([float("inf")], allow_nan=False)
except ValueError as e:
    print("ValueError:", str(e))
try:
    json.dumps({float("-inf"): 1}, allow_nan=False)
except ValueError as e:
    print("ValueError:", str(e))

# ── loads：重复键 last-wins / 严格数字文法 / 错误消息与位置 ──
dd = json.loads('{"a": 1, "b": 9, "a": 3}', {"": 0})
print(dd["a"], dd["b"])
print(json.loads("0e0", 0.0))
print(json.loads("1E5", 0.0))
print(json.loads("-0.0", 0.0))
print(json.loads("1e999", 0.0), json.loads("-1e999", 0.0))
print(json.loads("9223372036854775807", 0))
print(json.loads("-9223372036854775808", 0))
print(json.loads("  \n 1 \t ", 0))
print(json.loads('"\\/"', ""))
print(json.loads('"A中"', ""))
print(json.loads('"😀"', ""))

def bad(text: str, sample: int) -> None:
    try:
        json.loads(text, sample)
        print("ACCEPTED", text)
    except ValueError as e:
        print("ValueError:", str(e))

bad("01", 0)
bad("+1", 0)
bad("00", 0)
bad("0123", 0)
bad(".5", 0)
bad("-", 0)
bad("", 0)
bad("1 2", 0)
bad("1e", 0)

def badl(text: str) -> None:
    try:
        json.loads(text, [0])
        print("ACCEPTED", text)
    except ValueError as e:
        print("ValueError:", str(e))

badl("[1,]")
badl("[1 2]")
badl("[")

def badf(text: str, sample: float) -> None:
    try:
        json.loads(text, sample)
        print("ACCEPTED", text)
    except ValueError as e:
        print("ValueError:", str(e))

badf("1.", 0.0)
badf("0.", 0.0)
badf("inf", 0.0)
badf("nan", 0.0)
badf("0x1p3", 0.0)

def badd(text: str) -> None:
    try:
        json.loads(text, {"": 0})
        print("ACCEPTED", text)
    except ValueError as e:
        print("ValueError:", str(e))

badd('{"a":1,}')
badd('{"a" 1}')
badd("{1: 2}")
badd("{")
badd('{"a":1,,"b":2}')
badd('{\n  "a": 1,\n}')

def bads(text: str) -> None:
    try:
        json.loads(text, "")
        print("ACCEPTED", text)
    except ValueError as e:
        print("ValueError:", str(e))

bads('"abc')
bads('"a\tb"')
bads('"\\x41"')
bads('"\\u00"')
bads('"\\uZZZZ"')

# ── 往返 ──
s = json.dumps({"x": [1, 2], "y": [3]}, sort_keys=True)
r = json.loads(s, {"": [0]})
print(r["x"], r["y"])
s2 = json.dumps({"中文": "值"}, ensure_ascii=False)
r2 = json.loads(s2, {"": ""})
print(r2["中文"])

# ── 政策差异（CPython 不报错，pyadamas 按 int64/UTF-8 口径拒绝；钉住口径）──
# CPython 大整数无限精度；pyadamas int64 溢出报 ValueError 而非回绕。
try:
    json.loads("9223372036854775808", 0)
except ValueError as e:
    print("ValueError:", str(e))
try:
    json.loads("-9223372036854775809", 0)
except ValueError as e:
    print("ValueError:", str(e))
# CPython 接受孤立代理码点；pyadamas str 是 UTF-8，无法表示，拒绝。
try:
    json.loads('"\\ud800"', "")
except ValueError as e:
    print("ValueError:", str(e))


# --- expect ---
# {"1": "a", "2": "b"}
# {"2": 2, "10": 1}
# {"-5": 1, "0": 0, "3": 2}
# {"1.5": "x", "2.0": "y"}
# {"1.5": 2, "2.5": 1}
# {"true": 1, "false": 0}
# {"false": 2, "true": 1}
# {"null": 1}
# {"9223372036854775807": 1, "-9223372036854775808": 2}
# {"1": {"2": "x"}}
# {"1": [1, 2], "3": [4]}
# {
#   "1": [
#     1,
#     2
#   ]
# }
# {"0.1": 1, "1e+300": 2, "5e-324": 3}
# {"-0.0": 1}
# {"NaN": 1, "Infinity": 2, "-Infinity": 3}
# "\u4e2d\u6587"
# "中文"
# "a\u007fb"
# "ab"
# "\u0000\u001f\u000b"
# "\ud83d\ude00"
# "😀"
# {"键": "值"}
# "变量开关"
# {
# "a": 1
# }
# {
# "a": 1
# }
# [
# 1,
# 2
# ]
# {"a": 1}
# {}
# []
# {
#     "a": [
#         {
#             "b": 2
#         }
#     ]
# }
# [1,2]
# {"a":1,"b":2}
# [
#   1, 
#   2
# ]
# {"a" = 1}
# [12]
# {"a":[1,2]}
# NaN Infinity -Infinity
# [1.0, NaN, Infinity]
# ValueError: Out of range float values are not JSON compliant: nan
# ValueError: Out of range float values are not JSON compliant: inf
# ValueError: Out of range float values are not JSON compliant: -inf
# 3 9
# 0.0
# 100000.0
# -0.0
# inf -inf
# 9223372036854775807
# -9223372036854775808
# 1
# /
# A中
# 😀
# ValueError: Extra data: line 1 column 2 (char 1)
# ValueError: Expecting value: line 1 column 1 (char 0)
# ValueError: Extra data: line 1 column 2 (char 1)
# ValueError: Extra data: line 1 column 2 (char 1)
# ValueError: Expecting value: line 1 column 1 (char 0)
# ValueError: Expecting value: line 1 column 1 (char 0)
# ValueError: Expecting value: line 1 column 1 (char 0)
# ValueError: Extra data: line 1 column 3 (char 2)
# ValueError: Extra data: line 1 column 2 (char 1)
# ValueError: Illegal trailing comma before end of array: line 1 column 3 (char 2)
# ValueError: Expecting ',' delimiter: line 1 column 4 (char 3)
# ValueError: Expecting value: line 1 column 2 (char 1)
# ValueError: Extra data: line 1 column 2 (char 1)
# ValueError: Extra data: line 1 column 2 (char 1)
# ValueError: Expecting value: line 1 column 1 (char 0)
# ValueError: Expecting value: line 1 column 1 (char 0)
# ValueError: Extra data: line 1 column 2 (char 1)
# ValueError: Illegal trailing comma before end of object: line 1 column 7 (char 6)
# ValueError: Expecting ':' delimiter: line 1 column 6 (char 5)
# ValueError: Expecting property name enclosed in double quotes: line 1 column 2 (char 1)
# ValueError: Expecting property name enclosed in double quotes: line 1 column 2 (char 1)
# ValueError: Expecting property name enclosed in double quotes: line 1 column 8 (char 7)
# ValueError: Illegal trailing comma before end of object: line 2 column 9 (char 10)
# ValueError: Unterminated string starting at: line 1 column 1 (char 0)
# ValueError: Invalid control character at: line 1 column 3 (char 2)
# ValueError: Invalid \escape: line 1 column 2 (char 1)
# ValueError: Invalid \uXXXX escape: line 1 column 3 (char 2)
# ValueError: Invalid \uXXXX escape: line 1 column 3 (char 2)
# [1, 2] [3]
# 值
# ValueError: json: integer out of int64 range at offset 0
# ValueError: json: integer out of int64 range at offset 0
# ValueError: json: unpaired surrogate at offset 1
