"""io 基准：StringIO / BytesIO 写入与读取。"""
import io

total = 0
r = 0
while r < 300:
    s = io.StringIO()
    i = 0
    while i < 200:
        s.write("row %d\n" % i)
        i += 1
    v = s.getvalue()
    total += len(v)
    s.seek(0)
    total += len(s.readlines())
    s.close()
    b = io.BytesIO()
    i = 0
    while i < 100:
        b.write(b"abc")
        i += 1
    total += len(b.getvalue())
    r += 1
print(total)
