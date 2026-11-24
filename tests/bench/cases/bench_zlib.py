"""zlib 基准：compress / decompress / crc32 / adler32。"""
import zlib

text = "the quick brown fox jumps over the lazy dog " * 40
data = text.encode()

total = 0
r = 0
while r < 400:
    c = zlib.compress(data, 6)
    d = zlib.decompress(c)
    total += zlib.crc32(c) + zlib.adler32(d) + len(c) + len(d)
    r += 1
print(total)
