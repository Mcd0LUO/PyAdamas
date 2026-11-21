# 445: 推导式里迭代 dict/set 也要在容器大小变化时抛可捕获的 RuntimeError。
# 之前没有守卫，会落到 DictKeyAt 的越界 panic（进程退出、except 接不住）；
# 语句级 for 从 0.363 起就有守卫。


d: dict[int, int] = {1: 1, 2: 2, 3: 3}
try:
    xs = [d.pop(k) for k in d]
except RuntimeError as e:
    print("caught", e)

s: set[int] = set([1, 2])
ys = [x * 2 for x in s]
print(ys)

e: dict[str, int] = {"a": 1}
try:
    zs = [e.setdefault(k + "x", 1) for k in e]
except RuntimeError as err:
    print("caught2", err)
# --- expect ---
# caught dictionary changed size during iteration
# [2, 4]
# caught2 dictionary changed size during iteration
