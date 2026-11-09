# 字典/集合：字面量、下标读写、len、in、for、同构嵌套、函数参数
d = {"a": 1, "b": 2}
print(len(d))
print(d["a"])

d["c"] = 3
d["a"] = 10
print(len(d), d["a"], d["c"])
print("b" in d, "z" in d, "z" not in d)

total: int = 0
for k in d:
    total = total + len(k)
print(total)

s = {1, 2, 3, 3}
print(len(s), 2 in s, 5 in s, 5 not in s)
t: int = 0
for x in s:
    t = t + x
print(t)

print(2 in [1, 2, 3], "b" in ["a", "b"], "bc" in "abcd", "z" in "abcd")

nested = {"a": {"k": 1}, "b": {"k": 2}}
print(nested["a"]["k"], nested["b"]["k"])

def get(dd: dict[str, int], k: str) -> int:
    return dd[k]

print(get({"z": 9}, "z"))

empty = {}
empty["x"] = 1
print(empty["x"], len(empty))

f = {1: "one", 2: "two"}
print(f[1], 1 in f, 3 in f)

if d:
    print("d truthy")
if s:
    print("s truthy")

# --- expect ---
# 2
# 1
# 3 10 3
# True False True
# 3
# 3 True False True
# 6
# True True True False
# 1 2
# 9
# 1 1
# one True False
# d truthy
# s truthy
