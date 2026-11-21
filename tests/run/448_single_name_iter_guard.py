# 448: enumerate / zip 用**单个名字**作目标时也要有迭代守卫。
# 快速路径原先要求目标可解构（`for i, v in ...`），单名目标落到物化路径，
# 看不到源容器的长度变化 —— 迭代中改 dict 静默跑完。
# 现在单名目标也走快速路径：两个值由 bind_iter_pair 构成元组再赋值。


d: dict[str, int] = {"a": 1, "b": 2}
e: dict[str, int] = {"x": 3, "y": 4}
for pair in zip(d, e):
    print(pair[0], pair[1])

for kv in enumerate([10, 20]):
    print(kv[0], kv[1])

s: set[int] = set([1, 2])
try:
    for kv in enumerate(s):
        s.add(9)
except RuntimeError as err2:
    print("RE2", err2)

f: dict[str, int] = {"a": 1}
g: dict[str, int] = {"x": 2}
try:
    for pair in zip(f, g):
        f["n" + pair[0]] = 1
except RuntimeError as err:
    print("RE", err)
# --- expect ---
# a x
# b y
# 0 10
# 1 20
# RE2 Set changed size during iteration
# RE dictionary changed size during iteration
