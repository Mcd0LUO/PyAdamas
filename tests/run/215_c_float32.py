# C float（单精度）编组：.cpya 的 float32 种类。PyAdamas 侧仍是 double，
# 调用边界 cvtsd2ss/cvtss2sd；cdecl 现在也从 C 头文件生成 float32。
# 结构体字段同样支持 float32（4 字节/4 对齐，两个 float 共用一个 SSE 块）。
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

# float32 字段：构造（f64→f32 写入）、按值传入/返回、指针字段读写
q = fpair(1.5, 2.5)
print(q.x, q.y)
print(f.fpair_add(q))
r = f.fpair_make(2.5, 3.5)
print(r.x, r.y)
print(f.fpair_add(r))
s = f.fpair_ptr()
print(s.x, s.y, f.fpair_ptr_add(s))
s.x = 8.5
print(s.x, s.y, f.fpair_ptr_add(s))
# --- expect ---
# 3.375
# 3.5
# 3.1415927410125732
# 2.5
# 1.25 2.5 3.75
# 9.5
# 1.5 2.5
# 4.0
# 2.5 3.5
# 6.0
# 7.25 0.5 7.75
# 8.5 0.5 9.0
