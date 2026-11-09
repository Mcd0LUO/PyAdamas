# 推导式：list/set/dict、条件、多层、嵌套、range 与作用域隔离
sq: list[int] = [x * x for x in range(5)]
print(len(sq))
for v in sq:
    print(v)

lst: list[int] = [1, 2, 3, 4, 5, 6]
ev: list[int] = [x for x in lst if x % 2 == 0]
print(len(ev))
for v in ev:
    print(v)

pairs: list[tuple[int, int]] = [(x, y) for x in [1, 2] for y in [3, 4]]
print(len(pairs))
for p in pairs:
    print(p[0] * 10 + p[1])

d: dict[str, int] = {k: len(k) for k in ["a", "bb", "ccc"]}
print(d["a"], d["bb"], d["ccc"])

s: set[int] = {x % 3 for x in range(10)}
print(len(s), 0 in s, 1 in s, 2 in s)

nested: list[list[int]] = [[y for y in range(x)] for x in range(4)]
for row in nested:
    print(len(row))

x: int = 100
probe: list[int] = [x for x in range(3)]
print(x, len(probe))

gen: list[int] = (y + 1 for y in range(3))
print(len(gen), gen[0], gen[2])

d2: dict[str, int] = {"a": 1, "bb": 2}
ks: list[str] = [k for k in d2]
print(len(ks))
for k in ks:
    print(k)

chars: list[str] = [c for c in "ab"]
print(len(chars), chars[0], chars[1])
# --- expect ---
# 5
# 0
# 1
# 4
# 9
# 16
# 3
# 2
# 4
# 6
# 4
# 13
# 14
# 23
# 24
# 1 2 3
# 3 True True True
# 0
# 1
# 2
# 3
# 100 3
# 3 1 3
# 2
# a
# bb
# 2 a b
