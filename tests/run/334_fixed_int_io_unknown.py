# 334_fixed_int_io_unknown —— P1 定宽整数的输出/格式化/unknown 边界
#
# 覆盖：
#   * u64 的十进制输出（print/str/repr/f-string/format/%%d/hex/bin/oct）
#     —— ≥ 2^63 的值必须按无符号，不能带负号；
#   * 定宽 → float 的转换（u64 的 ≥ 2^63 走无符号转换路径）；
#   * 定宽过 unknown 边界**擦宽度提升 i64**（设计 §6.5 保守 v1）：
#     object/Any 形参收到 u64 后按 i64 位模式解释（u64 max 变成 -1），
#     回来必须显式 cast；这不是 bug，是 v1 的定死边界。

f: u64 = cast_u64(-1)
print(f)
print(str(f))
print(repr(f))
print(f"{f}")
print(format(f, "d"))
print("%d" % f)
print(hex(f))
print(bin(cast_u8(200)))
print(oct(cast_u16(65535)))
print(f / 2)
print(f / cast_u64(2))
print(float(f))
print(cast_i8(3) + 0.5)
print(cast_u32(4294967295) * 1.0)


def ident(x: object) -> object:
    return x


print(ident(cast_i8(-5)) == -5)
print(ident(cast_u8(200)) == 200)
print(ident(cast_u64(-1)) == -1)
print(ident(cast_u32(-1)) == 4294967295)


def shrink(x: object) -> u64:
    return cast_u64(x)


print(shrink(cast_u64(-2)))


def take(x: u64) -> bool:
    return x > cast_u64(1)


print(take(cast_u64(-1)))
print(f"{cast_u16(65535):>8}")
print(f"{cast_i8(-5):+d}")
print("%05.1f" % cast_i32(7))
# --- expect ---
# 18446744073709551615
# 18446744073709551615
# 18446744073709551615
# 18446744073709551615
# 18446744073709551615
# 18446744073709551615
# 0xffffffffffffffff
# 0b11001000
# 0o177777
# 9.223372036854776e+18
# 9.223372036854776e+18
# 1.8446744073709552e+19
# 3.5
# 4294967295.0
# True
# True
# True
# True
# 18446744073709551614
# True
#    65535
# -5
# 007.0
