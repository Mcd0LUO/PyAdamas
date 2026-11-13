# 按值结构体：C 返回小结构体、py23 构造结构体按值传参、值语义复制。
cimport clib as c

# div_t 8 字节，INTEGER 返回在 rax
d = c.div(7, 3)
print(d.quot)
print(d.rem)

# ldiv_t 16 字节，INTEGER chunk 依次在 rax/rdx
l = c.ldiv(-9, 4)
print(l.quot)
print(l.rem)

# in_addr 4 字节，按值走整数寄存器；C 返回 char* 复制成 str
a = in_addr(c.htonl(3232235777))
print(c.inet_ntoa(a))

# 结构体是值类型：赋值复制整块 payload
e = d
d.quot = 5
print(e.quot)
print(d.quot)

# --- expect ---
# 2
# 1
# -2
# -1
# 192.168.1.1
# 2
# 5
