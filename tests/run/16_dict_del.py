# dict/set 删除类方法：pop / popitem / remove / discard / clear
d: dict[str, int] = {"a": 1, "b": 2, "c": 3}
print(d.pop("b"))
print(len(d))
print(d.pop("z", 9))
d["d"] = 4
k, v = d.popitem()
print(k, v)
print(len(d))
for key in d:
    print(key)

s: set[int] = {1, 2, 3, 4}
print(s.pop())
s.remove(1)
s.discard(99)
print(len(s), 2 in s, 1 in s)

d.clear()
print(len(d), "a" in d)
s.clear()
print(len(s))

d2: dict[str, int] = {"0": 0, "1": 1, "2": 2, "3": 3, "4": 4, "5": 5}
d2.pop("0")
d2.pop("2")
d2.pop("4")
print(len(d2), d2["1"], d2["3"], d2["5"])
print(d2.popitem()[0])
print(len(d2))
# --- expect ---
# 2
# 2
# 9
# d 4
# 2
# a
# c
# 4
# 2 True False
# 0 False
# 0
# 3 1 3 5
# 5
# 2
