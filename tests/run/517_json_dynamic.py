# 517_json_dynamic.py —— 动态 JSON：json.parse / json.stringify + 访问辅助（0.447）
#
# LSP 场景：消息形状由对端决定，静态 sample 拿不到。json.parse(text) 返回
# unknown，运行期是 dict[str,unknown] / list[unknown] / 标量；null/bool 用
# 无出边的盒表示，所以 parse→stringify 往返不丢 true/false/null（和
# json.loads(sample) 的静态路径不同，这里不需要 sample）。
import json

# ── 往返：标量 / 容器 / 嵌套（对齐 CPython json.dumps(json.loads(text))）──
print(json.stringify(json.parse('{"a": 1, "b": 2.5, "c": true, "d": false, "e": null, "f": "s"}')))
print(json.stringify(json.parse("[1, [2, [3, {}]], []]")))
print(json.stringify(json.parse('{"x": {"y": [1, {"z": null}]}}')))
print(json.stringify(json.parse("null")), json.stringify(json.parse("true")),
      json.stringify(json.parse("false")))
print(json.stringify(json.parse("0")), json.stringify(json.parse("-9223372036854775808")))
print(json.stringify(json.parse("1e3")), json.stringify(json.parse("-0.0")),
      json.stringify(json.parse("1.5")))
print(json.stringify(json.parse('"a\\tb\\nc"')), json.stringify(json.parse('"\\u4e2d\\u6587"')))
print(json.stringify(json.parse('"\\ud83d\\ude00"')), json.stringify(json.parse('"héllo"')))

# 静态类型已知且同构时 stringify 走描述符路径（bool/float 精确）
print(json.stringify({"ok": True, "flag": False}))
print(json.stringify({"xs": [1.5, 2.0]}))
print(json.stringify([True, False]))
print(json.stringify(None), json.stringify(True), json.stringify(2.5))
# 异构容器（dict[str,unknown]）的 unknown 位置沿用语言既有口径：None/bool
# 是裸 0/1。要构造 JSON 的 true/false/null 用 json.bool()/json.null()。
print(json.stringify({"a": 1, "b": [True, None]}))
print(json.stringify({"a": 1, "b": [json.bool(True), json.null()]}))

# ── 动态访问（LSP 典型用法）────────────────────────────────────
text = '{"jsonrpc": "2.0", "id": 7, "method": "initialize", "params": {"rootUri": "file:///w", "capabilities": {"hover": true, "n": 3}}}'
msg = json.parse(text)
d = json.as_dict(msg)
print(d["jsonrpc"], d["id"], json.as_str(d["method"]))
print(json.as_str(json.path(msg, ["params", "rootUri"])))
print(json.as_int(json.path(msg, ["params", "capabilities", "n"])),
      json.as_bool(json.path(msg, ["params", "capabilities", "hover"])))
print(json.type(msg), json.type(d["id"]), json.type(d["method"]), json.type(d["params"]))
print(json.len(msg), json.keys(msg))
print(json.has(d, "method"), json.has(d, "nope"))
print(json.is_null(json.get_or(d, "nope", json.null())),
      json.is_null(json.get_or(d, "method", json.null())))
print(json.get_or(d, "nope", 42))

# 数组：as_list + 下标 / len / 迭代
arr = json.as_list(json.parse('[10, "x", true, null]'))
print(len(arr), arr[0], json.as_str(arr[1]), json.as_bool(arr[2]), json.is_null(arr[3]))
total = 0
for e in json.as_list(json.parse("[1, 2, 3]")):
    total += json.as_int(e)
print("sum", total)

# ── 构造 JSON-RPC 回复：unknown 位置上的 bool/null 用 json.bool/json.null ──
resp = {"jsonrpc": "2.0", "id": json.get(d, "id"),
        "result": {"ok": json.bool(True), "err": json.bool(False), "extra": json.null()}}
print(json.stringify(resp))

# ── from-import 别名（lower_json 同一条路径）────────────────────
from json import parse, stringify
print(stringify(parse('{"k": [1, null]}')))

# ── 端到端：二进制管道读 LSP 帧 → json.parse → 构造回复 ─────────
import subprocess

cmd = "printf 'Content-Length: 51\r\n\r\n{\"jsonrpc\": \"2.0\", \"id\": 1, \"method\": \"initialize\"}'"
p = subprocess.Popen(["sh", "-c", cmd], stdout=subprocess.PIPE)
f = p.stdout
header = f.readline()
blank = f.readline()          # 头部以空行结束（帧体前的 CRLF）
n = int(header.split(":")[1].strip())
raw = f.read(n)
p.wait()
req = json.parse(raw)
print("lsp:", len(blank) == 2, json.len(req), json.as_str(json.get(req, "method")))
reply = {"jsonrpc": "2.0", "id": json.get(req, "id"),
         "result": {"capabilities": {"hoverProvider": json.bool(True)}}}
print(json.stringify(reply))

# ── 错误输入：可捕获的异常 ─────────────────────────────────────
try:
    json.parse("{bad")
except ValueError as e:
    print("ValueError:", str(e))
for bad in ['[1,]', '{"a":}', 'nul', '1 2', '', '{"a" 1}']:
    try:
        json.parse(bad)
        print("no error")
    except ValueError:
        print("ValueError")
try:
    json.parse("9223372036854775808")
except ValueError as e:
    print("overflow:", str(e))
try:
    json.parse("[" * 600 + "]" * 600)
except ValueError as e:
    print("depth:", str(e))
try:
    json.as_str(d["id"])
except TypeError:
    print("as_str TypeError")
try:
    json.get(d, "missing")
except KeyError:
    print("KeyError")

# --- expect ---
# {"a": 1, "b": 2.5, "c": true, "d": false, "e": null, "f": "s"}
# [1, [2, [3, {}]], []]
# {"x": {"y": [1, {"z": null}]}}
# null true false
# 0 -9223372036854775808
# 1000.0 -0.0 1.5
# "a\tb\nc" "\u4e2d\u6587"
# "\ud83d\ude00" "h\u00e9llo"
# {"ok": true, "flag": false}
# {"xs": [1.5, 2.0]}
# [true, false]
# null true 2.5
# {"a": 1, "b": [1, 0]}
# {"a": 1, "b": [true, null]}
# 2.0 7 initialize
# file:///w
# 3 True
# dict int str dict
# 4 ['jsonrpc', 'id', 'method', 'params']
# True False
# True False
# 42
# 4 10 x True True
# sum 6
# {"jsonrpc": "2.0", "id": 7, "result": {"ok": true, "err": false, "extra": null}}
# {"k": [1, null]}
# lsp: True 3 initialize
# {"jsonrpc": "2.0", "id": 1, "result": {"capabilities": {"hoverProvider": true}}}
# ValueError: Expecting property name enclosed in double quotes: line 1 column 2 (char 1)
# ValueError
# ValueError
# ValueError
# ValueError
# ValueError
# ValueError
# overflow: json: integer out of int64 range at offset 0
# depth: maximum nesting depth exceeded (512)
# as_str TypeError
# KeyError
