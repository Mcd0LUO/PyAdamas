# json.dump / json.load（0.517）：文件流版。与 CPython 3.14.4 逐字节一致。
# load 返回 unknown（CPython 的返回类型是动态的，没有 sample 参数无法静态定），
# 所以回读结果用 repr 比较（repr(back) 与 repr(原对象) 两行分别打印）。
import json

d: dict[str, int] = {'a': 1, 'b': 2}
with open('/tmp/ji_a.json', 'w') as f:
    json.dump(d, f)
with open('/tmp/ji_a.json') as f:
    t = f.read()
print(t)
print(t == json.dumps(d))
with open('/tmp/ji_b.json', 'w') as f:
    json.dump(d, f, indent=2)
with open('/tmp/ji_b.json') as f:
    print(f.read())
with open('/tmp/ji_a.json') as f:
    back = json.load(f)
print(repr(back))
print(repr(back) == "{'a': 1, 'b': 2}")
xs: list[int] = [1, 2, 3]
with open('/tmp/ji_c.json', 'w') as f:
    json.dump(xs, f)
with open('/tmp/ji_c.json') as f:
    print(f.read())
with open('/tmp/ji_c.json') as f:
    ys = json.load(f)
print(repr(ys))
s: str = '中文\t\\'
with open('/tmp/ji_d.json', 'w') as f:
    json.dump(s, f)
with open('/tmp/ji_d.json') as f:
    print(f.read())
with open('/tmp/ji_bad.json', 'w') as f:
    f.write('{bad json}')
try:
    with open('/tmp/ji_bad.json') as f:
        json.load(f)
except ValueError as e:
    print('ValueError:', e)

# --- expect ---
# {"a": 1, "b": 2}
# True
# {
#   "a": 1,
#   "b": 2
# }
# {'a': 1, 'b': 2}
# True
# [1, 2, 3]
# [1, 2, 3]
# "\u4e2d\u6587\t\\"
# ValueError: Expecting property name enclosed in double quotes: line 1 column 2 (char 1)
