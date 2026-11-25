# 0.537：连续内存缓冲区（RC 管理，runtime pa_buffer）。
b = buffer(4)
print(len(b))
b[0] = 1.5
b[1] = 2.5
print(b[0], b[1])
print(b[3])

# O1：裸变量 / 表达式结果写入（曾经写成垃圾）
v: float = 3.75
x = buffer(2)
x[0] = v
print(x[0])
x[1] = v * 2.0
print(x[1])
x[0] = 1.0 + 2.0
print(x[0])

iv: int = 7
ib = buffer_i32(3)
ib[0] = iv
ib[1] = 9
print(len(ib), ib[0] + ib[1])

fb = buffer_f32(2)
fv: float = 1.5
fb[0] = fv
fb[1] = 2.25
print(fb[0] + fb[1])

# --- expect ---
# 4
# 1.5 2.5
# 0.0
# 3.75
# 7.5
# 3.0
# 3 16
# 3.75
