# 368_cimport_fixed_width —— P1 收尾（可选任务）：cimport 窄/无符号 C 类型
# 映射到定宽 TyKind：char/i8、short/i16、uchar/u8、ushort/u16、uint64(u)/u64。
# 覆盖：常量、参数/返回（符号/零扩展）、全局变量读写、ptr[T] 解引用、
# 按值结构体字段（1/2/8 字节宽）、回调跳板（C 侧窄参数按宽度提升）。
# 既有 int/int32/uint32 仍映射默认 int（提升语义不动）；窄化实参要 cast，
# 字面量范围内隐式 coerce（反例 f77/f78）。期望值按 C 语义模型计算。

cimport cfixedint as f

print(f.CMAX, f.UMAX, f.U64MAX)
print(f.char_id(-128), f.char_id(127), f.char_neg())
print(f.short_id(-32768), f.short_id(32767), f.short_min())
print(f.uchar_id(255), f.uchar_max())
print(f.ushort_id(65535), f.ushort_max())
print(f.u64_id(18446744073709551615), f.u64_max())
print(f.u64_add(18446744073709551615, 1))
print(f.char_global, f.ushort_global, f.u64_global)
f.char_global = 100
f.ushort_global = 1000
f.u64_global = 9223372036854775808
print(f.char_global, f.ushort_global, f.u64_global)
p = f.char_arr()
print(p[0], p[1], p[2])
q = f.u64_arr()
print(q[0], q[1])
m = f.struct_make(-1, -2, 255, 65535, 18446744073709551615)
print(m.c, m.s, m.uc, m.us, m.u64)


def cb(c: i8, s: i16, uc: u8, us: u16, u: u64) -> u64:
    return u + cast_u64(c) + cast_u64(s) + cast_u64(uc) + cast_u64(us)


print(f.cb_call(cb))
print(type(f.char_neg()) == int, isinstance(f.u64_max(), int))
# --- expect ---
# 127 255 18446744073709551615
# -128 127 -1
# -32768 32767 -32768
# 255 255
# 65535 65535
# 18446744073709551615 18446744073709551615
# 0
# -5 65535 18446744073709551615
# 100 1000 9223372036854775808
# -128 0 127
# 9223372036854775808 18446744073709551615
# -1 -2 255 65535 18446744073709551615
# 65786
# True True
