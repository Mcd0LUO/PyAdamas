# qsort：C 回调 + ptr[int] 标注，比较函数里直接解引用元素指针。
cimport clib as c

p = c.malloc_i(40)
p[0] = 5
p[1] = 1
p[2] = 4
p[3] = 2
p[4] = 3


def cmp(a: ptr[int], b: ptr[int]) -> int:
    if a[0] < b[0]:
        return -1
    if a[0] > b[0]:
        return 1
    return 0


c.qsort(p, 5, 8, cmp)
print(p[0])
print(p[1])
print(p[2])
print(p[3])
print(p[4])
c.free(p)

# --- expect ---
# 1
# 2
# 3
# 4
# 5
