# os.stat / os.lstat（0.518 C 部分）：内建记录对象 os.stat_result。
# 与 CPython 3.14.4 逐字节一致（大小、S_ISREG/S_ISDIR 的位判断、nlink/uid、OSError 文本）。
# 注：未知属性 `st.st_bogus` 在我们这里**编译期**报错（消息与 CPython 的运行时
# AttributeError 文本相同），所以不放运行时对拍。
import os

path = '/tmp/s2_stat_test.txt'
f = open(path, 'w')
f.write('hello stat')
f.close()
st = os.stat(path)
print(st.st_size)
print(st.st_mode & 61440 == 32768)
print(st.st_nlink >= 1)
print(st.st_uid == os.getuid())
st2 = os.lstat('/tmp')
print(st2.st_mode & 61440 == 16384)
try:
    os.stat('/nonexistent/x')
except OSError as e:
    print('OSError:', e)

# --- expect ---
# 10
# True
# True
# True
# True
# OSError: [Errno 2] No such file or directory: '/nonexistent/x'
