"""gzip 基准：compress / decompress（含 CRC 与长度校验）。"""
import gzip

data = b""
i = 0
while i < 200:
    data = data + ("line %d of the payload\n" % i).encode()
    i += 1

total = 0
r = 0
while r < 300:
    c = gzip.compress(data, 6)
    d = gzip.decompress(c)
    total += len(c) + len(d)
    r += 1
print(total)
