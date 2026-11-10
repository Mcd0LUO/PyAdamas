import json

print(json.dumps(42), json.dumps(True), json.dumps(None), json.dumps(1.5))
print(json.dumps("hi"))
print(json.dumps("a\"b\\c\nd"))
print(json.dumps("中文"))
print(json.dumps([1, 2, 3]))
print(json.dumps({"a": 1, "b": 2}))
print(json.dumps({"x": [1, 2], "y": [3]}))
print(json.dumps((1, "x", False)))
print(json.dumps(1.0), json.dumps(-0.0), json.dumps(1e300))

s = json.dumps({"x": [1, 2], "y": [3]})
print(s)
r = json.loads(s, {"": [0]})
print(r["x"], r["y"])
print(json.loads("[1,2,3]", [0]))
print(json.loads("\"hi\\nthere\"", ""))
print(json.loads("\"\\u4e2d\\u6587\"", ""))
t = json.loads("[1, \"x\"]", (0, ""))
print(t[0], t[1])
print(json.loads("true", False), json.loads("null", None), json.loads("2.5", 0.0))
try:
    json.loads("[1, \"x\"]", [0])
except ValueError:
    print("bad type ok")
try:
    json.loads("{bad", {"": 0})
except ValueError:
    print("syntax ok")

# --- expect ---
# 42 true null 1.5
# "hi"
# "a\"b\\c\nd"
# "\u4e2d\u6587"
# [1, 2, 3]
# {"a": 1, "b": 2}
# {"x": [1, 2], "y": [3]}
# [1, "x", false]
# 1.0 -0.0 1e+300
# {"x": [1, 2], "y": [3]}
# [1, 2] [3]
# [1, 2, 3]
# hi
# there
# 中文
# 1 x
# True None 2.5
# bad type ok
# syntax ok
