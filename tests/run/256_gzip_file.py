# gzip.open / GzipFile：二进制/文本读写、read 尺寸、readline/readlines/迭代、
# tell、上下文管理、关闭后错误、模式校验、通用换行。
import binascii
import gzip
from typing import cast


def hx(b: bytes) -> str:
    return binascii.hexlify(b).decode("utf-8")


p = "/tmp/pa256_gz.gz"
with gzip.open(p, "wb") as f:
    n = f.write(b"line1\nline2\nline3\n")
    print(n)
    print(f.tell())
with gzip.open(p, "rb") as f:
    print(hx(f.read()))
    print(f.tell())
with gzip.open(p, "rb") as f:
    print(hx(f.read(5)))
    print(hx(f.readline()))
    for ln in f.readlines():
        print(hx(ln))
with gzip.open(p, "rt") as f:
    print(f.read(), end="")
with gzip.open(p, "wt") as f:
    print(f.write("a\nb\n"))
with gzip.open(p, "rt") as f:
    for line in f:
        print(line, end="")
with gzip.open(p, "rb") as f:
    print(f.readable(), f.writable())
with gzip.open(p, "wb") as f:
    print(f.readable(), f.writable())

# 通用换行（文本读折叠 \r\n / \r）
with gzip.open(p, "wb") as f:
    f.write(b"x\r\ny\rz\n")
with gzip.open(p, "rt") as f:
    print(repr(f.read()))

# 多次写合并
with gzip.open(p, "wb") as f:
    f.write(b"part1-")
    f.write(b"part2")
with gzip.open(p, "rb") as f:
    print(hx(f.read()))

# 关闭后操作
f = gzip.open(p, "rb")
f.close()
print(f.closed)
try:
    f.read()
except ValueError as e:
    print("E1", str(e))
f = gzip.open(p, "wb")
f.close()
try:
    f.write(b"x")
except ValueError as e:
    print("E2", str(e))

# 只读/只写限制
f = gzip.open(p, "rb")
try:
    f.write(b"x")
except OSError as e:
    print("E3", str(e))
f.close()

# 模式校验
try:
    gzip.open(p, "rw")
except ValueError as e:
    print("E4", str(e))
try:
    gzip.open(p, "rtb")
except ValueError as e:
    print("E5", str(e))

# 读损坏文件
fb = open("/tmp/pa256_bad.gz", "wb")
fb.write(cast(str, b"not gzip"))
fb.close()
try:
    with gzip.open("/tmp/pa256_bad.gz", "rb") as f:
        f.read()
except gzip.BadGzipFile as e:
    print("E7", str(e))

# r/w 简写与 compresslevel
with gzip.open(p, "w", 1) as f:
    f.write(b"level1")
with gzip.open(p, "r") as f:
    print(hx(f.read()))
# --- expect ---
# 18
# 18
# 6c696e65310a6c696e65320a6c696e65330a
# 18
# 6c696e6531
# 0a
# 6c696e65320a
# 6c696e65330a
# line1
# line2
# line3
# 4
# a
# b
# True False
# False True
# 'x\ny\nz\n'
# 70617274312d7061727432
# True
# E1 I/O operation on closed file
# E2 I/O operation on closed file
# E3 [Errno 9] write() on read-only GzipFile object
# E4 must have exactly one of create/read/write/append mode
# E5 Invalid mode: 'rtb'
# E7 Not a gzipped file (b'no')
# 6c6576656c31
