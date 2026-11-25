# 0.536：连续内存缓冲区（RC 管理，runtime pa_buffer）。
b = buffer(4)
print(len(b))
b[0] = 1.5
b[1] = 2.5
print(b[0], b[1])
print(b[3])

ib = buffer_i32(3)
ib[0] = 7
ib[1] = 9
print(len(ib), ib[0] + ib[1])

fb = buffer_f32(2)
fb[0] = 1.5
fb[1] = 2.25
print(fb[0] + fb[1])

# --- expect ---
# 4
# 1.5 2.5
# 0.0
# 3 16
# 3.75
