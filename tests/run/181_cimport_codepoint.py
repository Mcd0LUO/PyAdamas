# cimport 字符串边界：传 str 给 C 的 char* 走 data 偏移（24），C 返回 char* 扫 pub
cimport clib as c

# 传 str 给 C：strlen 拿到的必须是 UTF-8 字节数（6），不是码点数（2）
print(c.strlen("中文"))
print(c.strlen("abc"))
print(c.strlen(""))

# 传含非 ASCII 的混合串
print(c.strlen("a中b文c"))

# C 返回 char*：复制成 PyAdamasStr 时要扫描算码点
name = c.getenv("PYADAMAS_NOT_SET_XYZ")
print(name is None)

# 传 bytes 给 C（BufBytes 的 data 偏移同样从 16 变 24）
print(len(b"\xe4\xb8\xad"), len_bytes(b"\xe4\xb8\xad"))

# 常量字符串（C 全局 char*）
print(c.NAME, len(c.NAME))

# --- expect ---
# 6
# 3
# 0
# 9
# True
# 3 3
# pyadamas 8
