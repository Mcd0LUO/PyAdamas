# expect: cannot take this buffer
# buffer_i32 是 4 字节元素，不能冒充 ptr[int]（8 字节）；错误信息要说清宽度。
cimport bufwidth as c

b = buffer_i32(4)
c.memset(b, 0, 16)
