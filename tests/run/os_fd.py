# os 文件描述符面（0.518 B 部分）：open/write/lseek/read/dup/fdopen 的完整
# round-trip。与 CPython 3.14.4 逐字节一致（含 OSError 的文本）。
import os

path = '/tmp/fd_roundtrip.txt'
fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 420)
print(os.write(fd, b'hello fd'))
print(os.lseek(fd, 0, 0))
os.close(fd)
fd = os.open(path, os.O_RDONLY)
print(os.read(fd, 5))
print(os.lseek(fd, 0, 0))
print(os.read(fd, 100))
fd2 = os.dup(fd)
print(os.lseek(fd2, 0, 0))
print(os.read(fd2, 3))
os.close(fd2)
os.close(fd)
fd = os.open(path, os.O_RDONLY)
f = os.fdopen(fd, 'r')
print(f.read())
f.close()
try:
    os.open('/nonexistent/x', os.O_RDONLY)
except OSError as e:
    print('OSError:', e)
# --- expect ---
# 8
# 0
# b'hello'
# 0
# b'hello fd'
# 0
# b'hel'
# hello fd
# OSError: [Errno 2] No such file or directory: '/nonexistent/x'
