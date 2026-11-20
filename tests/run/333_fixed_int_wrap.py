# 333_fixed_int_wrap —— P1 定宽整数的回绕/符号语义（**pa 语义**，不是 CPython）
#
# 定宽是 C 式二进制补码回绕，不是 CPython 的无限精度：
#   i8 127+1 → -128，u8 255+1 → 0，i32 2^31-1 + 1 → -2^31，u64 2^64-1 + 1 → 0。
# 同宽 signed+unsigned → unsigned（C 口径）：i8(-1) 与 u8 比较时按 255。
# 宽度 < 64 的无符号值在槽内恒非负（零扩展不变量），所以 signed 指令与
# 无符号指令在这些宽度上逐例等价；只有 u64 需要 divq/shrq/jb-ja。
# 每个值都是手算的 pa 期望（需要无限精度请用 P3 的 big，不在本用例范围）。

a: i8 = 127
b: i8 = 1
print(a + b)
print(cast_i8(-128) - cast_i8(1))
print(cast_i8(100) * cast_i8(3))
print(cast_i8(-128) // cast_i8(-1))
print(cast_i8(-128) % cast_i8(-1))
print(-cast_i8(-128))
print(~cast_i8(0))

u: u8 = 255
print(u + cast_u8(1))
print(u - cast_u8(1))
print(cast_u8(200) + cast_u8(100))
print(cast_u8(255) * cast_u8(2))
u2: u8 = 200
u2 += 100
print(u2)
u3: u8 = 0
u3 -= 1
print(u3)

s: i16 = 32767
print(s + cast_i16(1))
print(cast_i16(-32768) - cast_i16(1))
print(cast_u16(65535) + cast_u16(1))
print(cast_i32(2147483647) + cast_i32(1))
print(cast_u32(4294967295) + cast_u32(1))
print(cast_u32(4294967295) // cast_u32(2))
print(cast_u32(4294967295) % cast_u32(2))
print(9223372036854775807 + 1)
print(cast_u64(-1) + cast_u64(1))
print(cast_u64(-1) // cast_u64(2))
print(cast_u64(-1) % cast_u64(3))
print(cast_u64(-2) - cast_u64(3))
print(cast_u64(-1) > cast_u64(1))
print(cast_u64(-3) // cast_u64(5), cast_u64(-3) % cast_u64(5))

print(cast_i8(-1) < cast_u8(1))
print(cast_i8(-1) == cast_u8(255))
print(cast_i32(-1) + cast_u32(2))
print(cast_i32(-1) + cast_u32(1))
print(cast_i16(-1) > cast_u16(1))

print(cast_u8(1) << cast_u8(8))
print(cast_i8(-128) >> cast_i8(1))
print(cast_u8(128) >> cast_u8(1))
print(cast_i8(1) << cast_i8(7))
print(cast_u32(2147483648) >> cast_u32(1))

print(cast_i8(200))
print(cast_i8(-200))
print(cast_u8(-1))
print(cast_i16(65535))
print(cast_u16(-1))
print(cast_i32(2147483648))
print(cast_u32(-1))
print(cast_u64(-1))
print(cast_i64(cast_u32(-1)))
print(cast_u8(cast_i32(300)))
print(abs(cast_i8(-128)))
print(cast_i16(-7) // cast_i16(3), cast_i16(-7) % cast_i16(3))
print(cast_u8(200) // cast_u8(3), cast_u8(200) % cast_u8(3))
print(cast_i8(-7) // cast_i8(3), cast_i8(-7) % cast_i8(3))
z: i32 = 2000000000
print(z * cast_i32(2))
w: i8 = 100
w += 10
w += 10
w += 10
print(w)
# --- expect ---
# -128
# 127
# 44
# -128
# 0
# -128
# -1
# 0
# 254
# 44
# 254
# 44
# 255
# -32768
# 32767
# 0
# -2147483648
# 0
# 2147483647
# 1
# -9223372036854775808
# 0
# 9223372036854775807
# 0
# 18446744073709551611
# True
# 3689348814741910322 3
# False
# True
# 1
# 0
# True
# 0
# -64
# 64
# -128
# 1073741824
# -56
# 56
# 255
# -1
# 65535
# -2147483648
# 4294967295
# 18446744073709551615
# 4294967295
# 44
# 128
# -3 2
# 66 2
# -3 2
# -294967296
# -126
