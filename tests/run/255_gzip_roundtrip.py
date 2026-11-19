# gzip/zlib 字节级对拍：compress 各级别/mtime/空/全字节/大数据/多 member/
# 零填充/头标志（FNAME/FEXTRA/FHCRC/FCOMMENT）/错误路径/zlib 往返。
# bytes 打印用 hexlify（pyadamas print(bytes) 直出内容，CPython 打 repr）。
import binascii
import gzip
import zlib


def hx(b: bytes) -> str:
    return binascii.hexlify(b).decode("utf-8")


def le32(n: int) -> bytes:
    return bytes([n & 255, (n >> 8) & 255, (n >> 16) & 255, (n >> 24) & 255])


def trailer(data: bytes) -> bytes:
    return le32(zlib.crc32(data)) + le32(len(data))


d = b"hello hello hello world world"
for lvl in [0, 1, 6, 9]:
    c = gzip.compress(d, lvl)
    print(lvl, hx(c))
    print(gzip.decompress(c) == d)
print(hx(gzip.compress(d, mtime=123)))
print(hx(gzip.compress(b"")))
print(gzip.decompress(gzip.compress(b"")) == b"")

# 全字节值 0..255
allb = bytes(list(range(256)))
ca = gzip.compress(allb)
print(gzip.decompress(ca) == allb, len(ca))

# 大数据（重复 + 伪随机）
big = b"abcabcabc" * 5000
cb = gzip.compress(big, 6)
print(len(cb) < len(big), gzip.decompress(cb) == big)
lcg = 12345
rnd: list[int] = []
i = 0
while i < 4000:
    lcg = (lcg * 1664525 + 1013904223) % 4294967296
    rnd.append(lcg % 256)
    i += 1
rb = bytes(rnd)
cr = gzip.compress(rb)
print(gzip.decompress(cr) == rb)

# 多 member 与零填充
m = gzip.compress(b"a") + gzip.compress(b"b")
print(gzip.decompress(m))
m2 = gzip.compress(b"a") + b"\x00\x00\x00" + gzip.compress(b"b")
print(gzip.decompress(m2))

# 头标志 member（手工组装，zlib raw 流）
raw = zlib.compress(d)[2:-4]
hdr = b"\x1f\x8b\x08\x08" + b"\x00\x00\x00\x00" + b"\x02\xff" + b"file.txt\x00"
mem = hdr + raw + trailer(d)
print(gzip.decompress(mem) == d)
hc = b"\x1f\x8b\x08\x02" + b"\x00\x00\x00\x00" + b"\x02\xff"
hcrc = zlib.crc32(hc) & 0xFFFF
mem2 = hc + bytes([hcrc & 255, hcrc >> 8]) + raw + trailer(d)
print(gzip.decompress(mem2) == d)
hx2 = b"\x1f\x8b\x08\x04" + b"\x00\x00\x00\x00" + b"\x02\xff" + b"\x02\x00AB" + raw + trailer(d)
print(gzip.decompress(hx2) == d)
hcmt = b"\x1f\x8b\x08\x10" + b"\x00\x00\x00\x00" + b"\x02\xff" + b"comment\x00" + raw + trailer(d)
print(gzip.decompress(hcmt) == d)

# 错误路径
try:
    gzip.decompress(b"not gzip")
except gzip.BadGzipFile as e:
    print("E1", str(e))
try:
    gzip.decompress(b"\x1f")
except gzip.BadGzipFile as e:
    print("E2", str(e))
try:
    gzip.decompress(b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff")
except EOFError as e:
    print("E3", str(e))
try:
    gzip.decompress(gzip.compress(d)[:-2])
except EOFError as e:
    print("E4", str(e))
c0 = gzip.compress(b"abc")
lst: list[int] = list(c0)
lst[len(lst) - 8] = lst[len(lst) - 8] ^ 1
try:
    gzip.decompress(bytes(lst))
except gzip.BadGzipFile as e:
    print("E5", str(e))
lst2: list[int] = list(c0)
lst2[len(lst2) - 1] = lst2[len(lst2) - 1] ^ 1
try:
    gzip.decompress(bytes(lst2))
except gzip.BadGzipFile as e:
    print("E6", str(e))
try:
    gzip.decompress(b"\x1f\x8b\x09\x00" + b"\x00" * 6 + raw)
except gzip.BadGzipFile as e:
    print("E7", str(e))
try:
    gzip.decompress(gzip.compress(b"a") + b"garbage")
except gzip.BadGzipFile as e:
    print("E8", str(e))
try:
    gzip.compress(d, 99)
except zlib.error as e:
    print("E9", str(e))
try:
    raise gzip.BadGzipFile("x")
except OSError:
    print(True)

# zlib 往返与校验和
print(zlib.crc32(d), zlib.adler32(d))
print(zlib.crc32(d, 123))
zd = zlib.compress(d)
print(zlib.decompress(zd) == d)
print(zlib.decompress(zlib.compress(b"")) == b"")
print(zlib.decompress(zlib.compress(big, 9)) == big)
try:
    zlib.decompress(b"xx")
except zlib.error as e:
    print("E10", str(e))
try:
    zlib.decompress(zd[:-2])
except zlib.error as e:
    print("E11", str(e))
try:
    zlib.decompress(b"")
except zlib.error as e:
    print("E12", str(e))
# --- expect ---
# 0 1f8b08000000000004ff011d00e2ff68656c6c6f2068656c6c6f2068656c6c6f20776f726c6420776f726c64ee081d781d000000
# True
# 1 1f8b08000000000004ffcb48cdc9c957c84022cbf38b725214c02400ee081d781d000000
# True
# 6 1f8b08000000000000ffcb48cdc9c957c84022cbf38b7252202400ee081d781d000000
# True
# 9 1f8b08000000000002ffcb48cdc9c957c84022cbf38b7252202400ee081d781d000000
# True
# 1f8b08007b00000002ffcb48cdc9c957c84022cbf38b7252202400ee081d781d000000
# 1f8b08000000000002ff03000000000000000000
# True
# True 279
# True True
# True
# b'ab'
# b'ab'
# True
# True
# True
# True
# E1 Not a gzipped file (b'no')
# E2 Not a gzipped file (b'\x1f')
# E3 Compressed file ended before the end-of-stream marker was reached
# E4 Compressed file ended before the end-of-stream marker was reached
# E5 CRC check failed
# E6 Incorrect length of data produced
# E7 Unknown compression method
# E8 Not a gzipped file (b'ga')
# E9 Bad compression level
# True
# 2015168750 2759592717
# 1431308729
# True
# True
# True
# E10 Error -3 while decompressing data: incorrect header check
# E11 Error -5 while decompressing data: incomplete or truncated stream
# E12 Error -5 while decompressing data: incomplete or truncated stream
