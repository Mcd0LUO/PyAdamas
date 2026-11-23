# 527_union_array.py —— .cpya 的 union / 定长数组 / @packed（真实系统结构体）
cimport unionarr as c

print("epoll_event_size", c.ee_size())
print("epoll_data_off", c.ee_off_data())
print("timeval_size", c.tv_size())
print("sockaddr_un_size", c.su_size(), "path_off", c.su_off_path())

# 真实 epoll：注册 pipe 读端、写入、epoll_wait 读回事件
print("epoll_roundtrip", c.epoll_roundtrip())

# 数组字段：char[108] 读写下标 + 传给 C 求和
su = c.make_su()
c.su_fill(su, "abc")
print("path0", c.su_path0(su), "path_sum", c.su_path_sum(su, 3))
su.sun_path[0] = cast_i8(90)
print("path0_after_write", c.su_path0(su), "path_sum", c.su_path_sum(su, 3))

# 嵌套 struct 字段（timeval 内联在别处也可用）：字段读写
tv = c.make_tv()
c.tv_set(tv, 1700000000, 500000)
print("tv_sec", c.tv_get(tv))
tv.tv_usec = 123456
print("tv_usec", tv.tv_usec)

# union 读写：同一块内存按不同成员解释（偏移 0 + 各自宽度）
e = c.ee_make()
c.ee_set_u64(e, 0x1122334455667788)
print('u64->u32', hex(c.ee_get_u32(e)), 'u64->fd', c.ee_get_fd(e))
e.data.u32 = 0x0A0B0C0D
print('u32_write->u32', hex(e.data.u32), 'u32->u64', hex(e.data.u64))
# --- expect ---
# epoll_event_size 12
# epoll_data_off 4
# timeval_size 16
# sockaddr_un_size 110 path_off 2
# epoll_roundtrip 1
# path0 97 path_sum 294
# path0_after_write 90 path_sum 287
# tv_sec 1700000000
# tv_usec 123456
# u64->u32 0x55667788 u64->fd 1432778632
# u32_write->u32 0xa0b0c0d u32->u64 0x112233440a0b0c0d
