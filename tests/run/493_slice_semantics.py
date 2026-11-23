# 切片/下标语义回归（0.427）：负区间切片赋值的插入点、del 扩展切片、
# 扩展切片长度不匹配文案、基本切片 clamp/步长/负索引、slice() 内联对象。

# 1) 负区间（start > stop）切片赋值：空切片，插入点是 start（曾经静默错值）
a = [0, 1, 2, 3, 4, 5, 6]
a[-3:-8] = [90]
print("neg_range_ins", a)
b = [0, 1, 2, 3, 4, 5, 6]
b[-3:-8] = []
print("neg_range_empty", b)
c = [0, 1, 2, 3, 4, 5, 6]
c[-3:-8] = [90, 91, 92]
print("neg_range_multi", c)
d = [0, 1, 2, 3, 4, 5, 6]
d[100:200] = [7, 8]
print("clamp_ins", d)
e = [0, 1, 2, 3, 4, 5, 6]
e[-100:-200] = [1, 2]
print("neg_clamp_ins", e)
f = [0, 1, 2, 3, 4, 5, 6]
f[6:-100] = [7]
print("mixed_ins", f)

# 2) 等长/不等长/插入/反转赋值（回归）
g = [0, 1, 2, 3, 4]
g[1:3] = [9, 8]
print("eq_len", g)
h = [0, 1, 2, 3, 4]
h[1:3] = [9]
print("shrink", h)
i = [0, 1, 2, 3, 4]
i[1:3] = [7, 7, 7]
print("grow", i)
j = [0, 1, 2, 3, 4]
j[2:2] = [5, 6]
print("insert", j)
k = [0, 1, 2, 3, 4]
k[::-1] = [9, 8, 7, 6, 5]
print("rev_assign", k)

# 3) 扩展切片长度不匹配：ValueError 文案与 CPython 一致（带尺寸）
m = [0, 1, 2, 3, 4, 5, 6]
try:
    m[::2] = [1, 2]
except ValueError as err:
    print("ext_shorterr", str(err))
n = [0, 1, 2, 3, 4, 5, 6]
try:
    n[::-2] = [1]
except ValueError as err:
    print("ext_revshorterr", str(err))

# 4) del 切片（含扩展步长；以前 del a[::2] 直接抛 ValueError）
o = [0, 1, 2, 3, 4, 5, 6]
del o[1:3]
print("del_basic", o)
p = [0, 1, 2, 3, 4, 5, 6]
del p[::2]
print("del_step", p)
q = [0, 1, 2, 3, 4, 5, 6]
del q[::-2]
print("del_negstep", q)
r = [0, 1, 2, 3, 4, 5, 6]
del r[5:2]
print("del_empty", r)
s = [0, 1, 2]
del s[:]
print("del_all", s)

# 5) 切片读：clamp/负索引/步长/空容器/slice() 内联对象
xs = [0, 1, 2, 3, 4, 5, 6]
print("read", xs[1:3], xs[:2], xs[2:], xs[:], xs[5:2], xs[0:100])
print("read_neg", xs[-2:], xs[:-2], xs[-100:100], xs[100:200])
print("read_step", xs[::2], xs[1::2], xs[::-1], xs[3:1:-1], xs[::-2])
print("read_empty", [][:], [][0:1], [][-1:])
print("slice_obj", xs[slice(1, 3)], xs[slice(None, None, -1)], xs[slice(0, 5, 2)])
print("str_read", "abcdef"[1:3], "abcdef"[::-1], "abc"[0:100], "abc"[5:2])
print("tuple_read", (1, 2, 3, 4)[1:3], (1, 2, 3, 4)[::-1])

print("done", len(xs))

# --- expect ---
# neg_range_ins [0, 1, 2, 3, 90, 4, 5, 6]
# neg_range_empty [0, 1, 2, 3, 4, 5, 6]
# neg_range_multi [0, 1, 2, 3, 90, 91, 92, 4, 5, 6]
# clamp_ins [0, 1, 2, 3, 4, 5, 6, 7, 8]
# neg_clamp_ins [1, 2, 0, 1, 2, 3, 4, 5, 6]
# mixed_ins [0, 1, 2, 3, 4, 5, 7, 6]
# eq_len [0, 9, 8, 3, 4]
# shrink [0, 9, 3, 4]
# grow [0, 7, 7, 7, 3, 4]
# insert [0, 1, 5, 6, 2, 3, 4]
# rev_assign [5, 6, 7, 8, 9]
# ext_shorterr attempt to assign sequence of size 2 to extended slice of size 4
# ext_revshorterr attempt to assign sequence of size 1 to extended slice of size 4
# del_basic [0, 3, 4, 5, 6]
# del_step [1, 3, 5]
# del_negstep [1, 3, 5]
# del_empty [0, 1, 2, 3, 4, 5, 6]
# del_all []
# read [1, 2] [0, 1] [2, 3, 4, 5, 6] [0, 1, 2, 3, 4, 5, 6] [] [0, 1, 2, 3, 4, 5, 6]
# read_neg [5, 6] [0, 1, 2, 3, 4] [0, 1, 2, 3, 4, 5, 6] []
# read_step [0, 2, 4, 6] [1, 3, 5] [6, 5, 4, 3, 2, 1, 0] [3, 2] [6, 4, 2, 0]
# read_empty [] [] []
# slice_obj [1, 2] [6, 5, 4, 3, 2, 1, 0] [0, 2, 4]
# str_read bc fedcba abc 
# tuple_read (2, 3) (4, 3, 2, 1)
# done 7
