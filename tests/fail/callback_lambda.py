# expect: lambda cannot be used as a C callback
# C 回调必须是命名函数：lambda 的形参没有类型，体内 a[0] 会被当成通用索引。
cimport clib as c

xs: list[int] = [3, 1, 2]
c.qsort(xs, 3, 8, lambda a, b: a[0] - b[0])
print(xs)
