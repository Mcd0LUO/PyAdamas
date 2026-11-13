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
print(c.name_of(0))
print(c.name_of(1))

p = c.make_point(3, 4)
print(p.x, p.y)

buf = c.alloc_ints(3)
c.fill_buffer(buf, 3, 5)
print(buf[0], buf[1], buf[2])
c.free(buf)

print(c.global_counter)
c.global_counter = 9
print(c.global_counter)
print(c.global_ratio)

# --- expect ---
# 42
# 1.5
# 5
# 6
# 5
# 3
# 60
# 3.0
# zero
# other
# 3 4
# 5 6 7
# 7
# 9
# 2.5
