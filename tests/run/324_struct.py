# struct 子集：pack/unpack/calcsize/iter_unpack（std 字节序）
import struct

print(struct.calcsize("<i"))
print(struct.calcsize(">h"))
print(struct.calcsize("2i"))
print(struct.calcsize("4s"))
print(struct.calcsize("<?b"))
print(struct.calcsize("i"))
print(struct.calcsize("=i"))
print(struct.calcsize("@bi"))

print(struct.pack("<i", 42))
print(struct.pack("<i", -1))
print(struct.pack(">h", -2))
print(struct.pack("<2i", 1, 2))
print(struct.pack(">2H", 258, 65535))
print(struct.pack("<q", -9223372036854775807))
print(struct.pack("<b", -5), struct.pack("<B", 255))
print(struct.pack("<?", True), struct.pack("<?", False))
print(struct.pack("<c", b"Z"))
print(struct.pack("<4s", b"ab"))
print(struct.pack("<4s", b"abcd"))
print(struct.pack("<4p", b"hey"))
print(struct.pack("<d", 1.5))
print(struct.pack("<d", -0.0))
print(struct.pack("<f", 1.5))
print(struct.pack("<f", -2.25))
print(struct.pack("<3x"))
print(struct.pack("<b2x", 7))

print(struct.unpack("<i", struct.pack("<i", 42)))
print(struct.unpack("<i", struct.pack("<i", -42)))
print(struct.unpack(">H", struct.pack(">H", 65535)))
print(struct.unpack("<d", struct.pack("<d", 1.5)))
print(struct.unpack("<f", struct.pack("<f", 1.5)))
print(struct.unpack("<2i", struct.pack("<2i", 3, 4)))
def text(raw: bytes) -> str:
    return raw.decode("latin-1").replace("\x00", "")

t = struct.unpack("<4s", b"ab\x00\x00")
print(text(t[0]))
t = struct.unpack("<c", b"Z")
print(text(t[0]))
t = struct.unpack("<?", struct.pack("<?", True))
print(bool(t[0]))
print(struct.unpack_from("<2i", struct.pack("<2i", 9, 8), 0))

rec = struct.pack("<i", 1) + struct.pack("<i", 2) + struct.pack("<i", 3)
for t in struct.iter_unpack("<i", rec):
    print(t[0])

print(struct.unpack("<B", b"\xff"))
try:
    struct.unpack("<i", b"ab")
except struct.error:
    print("short buffer")
try:
    struct.pack("<i", 1, 2)
except struct.error:
    print("extra args")
try:
    struct.calcsize("<z")
except struct.error:
    print("bad code")


print(struct.calcsize("@ld"), struct.calcsize("=l"), struct.calcsize("=d"))
print(struct.pack("<f", 0.1))
print(struct.pack("<d", 1e300))
print(struct.unpack("<d", struct.pack("<d", 1e300))[0])
print(struct.unpack("<d", struct.pack("<d", -1e-300))[0])
print(struct.unpack("<f", struct.pack("<f", 0.1))[0])
print(struct.pack("<5p", b"abcdef"))
t = struct.unpack("<5p", struct.pack("<5p", b"abcdef"))
print(text(t[0]))
v = struct.unpack("<id", struct.pack("<id", 5, 2.5))
print(v[0], v[1])
print(struct.unpack_from("<h", b"xx" + struct.pack("<h", 300), 2))


# --- expect ---
# 4
# 2
# 8
# 4
# 2
# 4
# 4
# 8
# b'*\x00\x00\x00'
# b'\xff\xff\xff\xff'
# b'\xff\xfe'
# b'\x01\x00\x00\x00\x02\x00\x00\x00'
# b'\x01\x02\xff\xff'
# b'\x01\x00\x00\x00\x00\x00\x00\x80'
# b'\xfb' b'\xff'
# b'\x01' b'\x00'
# b'Z'
# b'ab\x00\x00'
# b'abcd'
# b'\x03hey'
# b'\x00\x00\x00\x00\x00\x00\xf8?'
# b'\x00\x00\x00\x00\x00\x00\x00\x80'
# b'\x00\x00\xc0?'
# b'\x00\x00\x10\xc0'
# b'\x00\x00\x00'
# b'\x07\x00\x00'
# (42,)
# (-42,)
# (65535,)
# (1.5,)
# (1.5,)
# (3, 4)
# ab
# Z
# True
# (9, 8)
# 1
# 2
# 3
# (255,)
# short buffer
# extra args
# bad code
# 16 4 8
# b'\xcd\xcc\xcc='
# b'\x9cu\x00\x88<\xe47~'
# 1e+300
# -1e-300
# 0.10000000149011612
# b'\x04abcd'
# abcd
# 5 2.5
# (300,)
