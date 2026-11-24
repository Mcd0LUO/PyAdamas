# cdecl 生成声明文件的端到端测试：sample.cpya 由 sample.h 自动生成。
cimport sample as c

print(c.MAGIC)
print(c.RATIO)
print(c.GREEN)
print(c.BLUE)

print(c.add(2, 3))
print(c.mix(3, 5))

xs = [10, 20, 30]
print(c.sum_ptrs(xs, 3))
print(c.scale(2.0, 1.5))
# C float（单精度）：cdecl 生成 float32，边界做 double↔float 转换
print(c.scalef(2.0, 1.5))
print(c.name_of(0))
print(c.name_of(1))

p = c.make_point(3, 4)
print(p.x, p.y)

# float32 字段结构体：按值返回、按值传参、指针字段读写
fp = c.fpoint_make(1.5, -2.25)
print(fp.x, fp.y)
fp2 = c.fpoint_add(fp, c.fpoint_make(0.25, 2.5))
print(fp2.x, fp2.y)
fr = c.fpoint_ref()
print(fr.x, fr.y)
fr.x = 4.5
print(fr.x, c.fpoint_ref().x)

buf = c.alloc_ints(3)
c.fill_buffer(buf, 3, 5)
print(buf[0], buf[1], buf[2])
c.free(buf)

print(c.global_counter)
c.global_counter = 9
print(c.global_counter)
print(c.global_ratio)
print(c.global_ratiof)

# 真实头文件形态：导出宏 + __attribute__ + 跨头文件 #include
print(c.extra_twice(21))


def cb(x, s):
    return x + 1


print(c.extra_call(cb, 41))
h = c.extra_open()
print(h != 0)
np = c.extra_make_pair(3, 9)
print(np.a, np.b)

# --- expect ---
# 42
# 1.5
# 5
# 6
# 5
# 3
# 60
# 3.0
# 3.0
# zero
# other
# 3 4
# 1.5 -2.25
# 1.75 0.25
# 7.25 0.5
# 4.5 4.5
# 5 6 7
# 7
# 9
# 2.5
# 1.25
# 42
# 42
# True
# 3 9
