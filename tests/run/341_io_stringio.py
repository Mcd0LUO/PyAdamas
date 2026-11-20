# pyadamas: lang-gaps
"""341: P4 block2 — io.StringIO/BytesIO + file protocol generalization.

CPython parity: byte-identical output. Avoided pa boundary: repr of
strings with control chars (pa str repr does not escape hex escapes —
pre-existing); NUL padding is verified via len/ord instead.
"""
import csv
from io import StringIO, BytesIO
from contextlib import closing


class MyBuffer:
    """User-defined FileLike: read/write method face, file-param dispatch."""

    data: str
    pos: int

    def __init__(self) -> None:
        self.data = ""
        self.pos = 0

    def read(self, size: int = -1) -> str:
        s: str = self.data[self.pos:]
        self.pos = len(self.data)
        return s

    def write(self, s: str) -> int:
        self.data = self.data + s
        self.pos = len(self.data)
        return len(s)

    def seek(self, offset: int, whence: int = 0) -> int:
        self.pos = offset
        return self.pos

    def tell(self) -> int:
        return self.pos

    def close(self) -> None:
        self.pos = 0


def consume(f: file) -> str:
    f.write("via-param\n")
    f.seek(0)
    return f.read()


def lines_of(f: file) -> list[str]:
    out: list[str] = []
    for line in f:
        out.append(line)
    return out


# ── StringIO basics ─────────────────────────────────────────────
s = StringIO()
print(s.write("hello"))
print(s.write(" world"))
print(s.getvalue())
print(s.tell())
print(s.seek(0))
print(s.read(5))
print(s.read())
print(s.read())
print(s.seek(0, 2))
print(s.seek(6))
print(s.read())
print(s.seek(0, 1))

s2 = StringIO("abc\ndef\nghi")
print(s2.readline())
print(s2.readlines())
s2.seek(0)
print(s2.read())
s2.seek(0)
print(s2.readline(2))
print(s2.tell())

# overwrite middle / write past end (NUL padding, CPython semantics)
s3 = StringIO("abcdef")
s3.seek(2)
s3.write("XY")
print(s3.getvalue())
s6 = StringIO("hello")
s6.seek(10)
print(s6.write("X"))
v: str = s6.getvalue()
print(len(v))
print(ord(v[5]), ord(v[9]), v[10])

# truncate
s4 = StringIO("abcdef")
s4.seek(3)
print(s4.truncate())
print(s4.getvalue())
print(s4.truncate(1))
print(s4.getvalue())

# writelines
s7 = StringIO()
s7.writelines(["a\n", "b\n", "c"])
print(s7.getvalue())

# ── error faces (messages byte-identical to CPython) ────────────
try:
    StringIO("x").seek(-1)
except ValueError as e:
    print("seekneg", str(e))
try:
    StringIO("x").seek(0, 3)
except ValueError as e:
    print("whence", str(e))
try:
    StringIO("x").seek(1, 2)
except OSError as e:
    print("endrel", str(e))
# truncate(显式负参) 按默认处理（pa int 默认参限制，见 io.py 文档）；
# 这里只钉 truncate() 与 truncate(非负) 的 CPython 一致口径。
s5 = StringIO("x")
print(s5.closed)
s5.close()
print(s5.closed)
try:
    s5.read()
except ValueError as e:
    print("closed err", str(e))
try:
    s5.write("y")
except ValueError as e:
    print("closed write", str(e))

# ── with / closing ──────────────────────────────────────────────
with StringIO("ctx") as sw:
    print(sw.read())
print(sw.closed)
sc = StringIO("closing")
with closing(sc):
    print(sc.read())
print(sc.closed)

# ── csv interop ─────────────────────────────────────────────────
buf = StringIO()
cw = csv.writer(buf)
cw.writerow(["a", "b", "c"])
cw.writerow(["1", "2,3", "x\"y"])
cw.writerows([["p", "q"], ["r", "s"]])
print(repr(buf.getvalue()))
buf.seek(0)
for row in csv.reader(buf):
    print(row)

# ── print(file=...) ─────────────────────────────────────────────
out = StringIO()
print("printed", 42, file=out)
print("second", file=out, end="!")
print(repr(out.getvalue()))

# ── file-param dispatch (StringIO + user FileLike) ──────────────
print(repr(consume(StringIO())))
print(repr(consume(MyBuffer())))
print(lines_of(StringIO("l1\nl2\nl3")))

# ── BytesIO ─────────────────────────────────────────────────────
b = BytesIO()
print(b.write(b"\x00\x01\x02"))
print(b.getvalue())
b.seek(1)
print(b.read(1))
print(b.tell())
print(b.seek(0, 2))
print(b.read())
bb = BytesIO(b"line1\nline2\n")
print(bb.readline())
print(bb.readlines())
bb.seek(0)
print(bb.read())
bt = BytesIO(b"abcdef")
bt.seek(2)
print(bt.truncate())
print(bt.getvalue())
with BytesIO(b"wctx") as bw:
    print(bw.read())
print(bw.closed)
print("done")
# --- expect ---
# 5
# 6
# hello world
# 11
# 0
# hello
#  world
#
# 11
# 6
# world
# 11
# abc
#
# ['def\n', 'ghi']
# abc
# def
# ghi
# ab
# 2
# abXYef
# 1
# 11
# 0 0 X
# 3
# abc
# 1
# a
# a
# b
# c
# seekneg Negative seek position -1
# whence Invalid whence (3, should be 0, 1 or 2)
# endrel Can't do nonzero cur-relative seeks
# False
# True
# closed err I/O operation on closed file
# closed write I/O operation on closed file
# ctx
# True
# closing
# True
# 'a,b,c\r\n1,"2,3","x""y"\r\np,q\r\nr,s\r\n'
# ['a', 'b', 'c']
# ['1', '2,3', 'x"y']
# ['p', 'q']
# ['r', 's']
# 'printed 42\nsecond!'
# 'via-param\n'
# 'via-param\n'
# ['l1\n', 'l2\n', 'l3']
# 3
# b'\x00\x01\x02'
# b'\x01'
# 2
# 3
# b''
# b'line1\n'
# [b'line2\n']
# b'line1\nline2\n'
# 2
# b'ab'
# b'wctx'
# True
# done
