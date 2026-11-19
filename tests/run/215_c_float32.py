# C float（单精度）编组：.cpya 的 float32 种类。PyAdamas 侧仍是 double，
# 调用边界 cvtsd2ss/cvtss2sd；cdecl 现在也从 C 头文件生成 float32。
cimport cfloat32 as f

print(f.f32_mul(1.5, 2.25))
print(f.f32_of_int(7))
# double → float → double：能看到单精度舍入
print(f.f32_roundtrip(3.141592653589793))
print(f.f32_global)
p = f.f32_arr_ptr()
print(p[0], p[1], p[2])
p[1] = 9.5
print(p[1])
# --- expect ---
# 3.375
# 3.5
# 3.1415927410125732
# 2.5
# 1.25 2.5 3.75
# 9.5
