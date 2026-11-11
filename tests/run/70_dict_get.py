# d.get(k) 缺失时返回 None，不再抛 KeyError
d = {"a": 1}
print(d.get("a"))
print(d.get("zz"))
print(d.get("zz", 42))
v = d.get("a")
print(v + 1)
w = d.get("zz")
print(w)
if w is not None:
    print("not none")
else:
    print("is none")

# 已知偏差：py23 里 None 就是 0，值为 0 的项和缺失分不出来
# （CPython 这里打印 0）
z = {"n": 0}
print(z.get("n"))

# --- expect ---
# 1
# None
# 42
# 2
# None
# is none
# None
