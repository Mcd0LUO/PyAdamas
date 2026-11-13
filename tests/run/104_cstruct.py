# .cpya 里的 C 结构体：ptr[Struct] 按编译期偏移读写字段。
cimport clib as c

p = c.malloc(8)
c.memset(p, 0, 8)
tm = c.gmtime(p)

print(tm.tm_year)
print(tm.tm_mon)
print(tm.tm_mday)
print(tm.tm_sec)

# 4 字节有符号字段写回
tm.tm_year = -5
print(tm.tm_year)
tm.tm_year = 70
print(tm.tm_year)

# 嵌套指针字段（tm_zone: char*）是 Optional[int]，可判空
if tm.tm_zone is None:
    print("no zone")
else:
    print("zone")

c.free(p)

# --- expect ---
# 70
# 0
# 1
# 0
# -5
# 70
# zone
