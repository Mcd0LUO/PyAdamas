# enumerate / zip 的 for 脱糖：直接计数循环，不建 [下标, 值] 的元组表
T = [10, 20, 30]
U = [1, 2]

for i, v in enumerate(T):
    print(i, v)

for a, b in zip(T, U):
    print(a, b)

for i, ch in enumerate("abc"):
    print(i, ch)

d = {"x": 1, "y": 2}
for i, k in enumerate(d):
    print(i, k)

for i, v in enumerate(T):
    if i == 1:
        continue
    print("skip", i)
else:
    print("done")

for a, b in zip(T, U):
    if a == 20:
        break
    print("pair", a, b)

# 空序列
for i, v in enumerate([]):
    print("never")
for a, b in zip([], T):
    print("never")

# 嵌套解构：退回物化路径，语义不变
pairs = [(1, 2), (3, 4)]
for idx, (x, y) in enumerate(pairs):
    print(idx, x, y)

# 当普通值用：仍然物化成列表（可 len、可下标）
e = enumerate([7, 8])
print(len(e), e[0][0], e[0][1])
# --- expect ---
# 0 10
# 1 20
# 2 30
# 10 1
# 20 2
# 0 a
# 1 b
# 2 c
# 0 x
# 1 y
# skip 0
# skip 2
# done
# pair 10 1
# 0 1 2
# 1 3 4
# 2 0 7
