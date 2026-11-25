# 整数宽度与位运算（0.536）：int 是 i64（算术右移、回绕），定宽无符号类型给逻辑语义。
# 文档：docs/language.md「整数宽度、溢出与位运算」。

b: u32 = 4294967295
b = cast_u32(b + 1)
print(b)

c: u64 = 18446744073709551615
c = cast_u64(c + 1)
print(c)

d: u32 = 2147483648
print(d >> 1)

e: i64 = -8
print(e >> 1)

print(cast_u32(-1))

MASK32: int = 0xFFFFFFFF
crc: int = -1
crc = (crc >> 1) & MASK32
print(crc)

# --- expect ---
# 0
# 0
# 1073741824
# -4
# 4294967295
# 4294967295
