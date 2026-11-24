"""struct 基准：pack / unpack 常用格式。"""
import struct

fmt = ">Ihi4sd"
total = 0
r = 0
while r < 4000:
    b = struct.pack(fmt, 123456789, -300, 7, b"abcd", 1.5)
    t = struct.unpack(fmt, b)
    total += len(b) + t[0] + t[1] + t[2] + t[4]
    r += 1
print(total)
print(len(struct.pack("<3i", 1, 2, 3)), struct.calcsize(">Ihi4sd"))
