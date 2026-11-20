# 363 tarfile 的压缩与错误面：w:gz/r:gz 往返、r:* 嗅探（明文/gz）、
# r: 显式明文、a 追加、x 已存在、gzip 层互操作（gz 解压载荷与明文归档
# 逐字节相同）、坏校验和/截断/非 tar/非 gz/未知压缩的错误类型。
# 对拍 CPython 3.14：输出逐字节一致（gz 压缩字节不复现，只比解压载荷）。
# pyadamas: lang-gaps
import gzip
import os.path as _osp
import tempfile
import tarfile
from io import BytesIO
from tarfile import TarInfo
from typing import cast


d = tempfile.mkdtemp()

# ── 内存建一份「明文归档字节」（后面所有对拍都用它）───────────────
mem = BytesIO()
tm = tarfile.open(fileobj=mem, mode="w")
a = TarInfo("a.txt")
a.size = 5
a.mtime = 1700000000
a.mode = 0o644
tm.addfile(a, BytesIO(b"hello"))
b = TarInfo("sub/b.txt")
b.size = 3
b.mtime = 1700000001
tm.addfile(b, BytesIO(b"xyz"))
sub = TarInfo("sub")
sub.type = tarfile.DIRTYPE
sub.mtime = 1700000000
tm.addfile(sub)
tm.close()
plain_raw: bytes = mem.getvalue()
print(len(plain_raw))

pp = d + "/plain.tar"
f = open(pp, "wb")
f.write(cast(str, plain_raw))
f.close()
print(_osp.getsize(pp))

# ── w:gz / r:gz ────────────────────────────────────────────────
p = d + "/g.tar.gz"
tf = tarfile.open(p, "w:gz")
g1 = TarInfo("a.txt")
g1.size = 5
g1.mtime = 1700000000
g1.mode = 0o644
tf.addfile(g1, BytesIO(b"hello"))
g2 = TarInfo("sub/b.txt")
g2.size = 3
g2.mtime = 1700000001
tf.addfile(g2, BytesIO(b"xyz"))
g3 = TarInfo("sub")
g3.type = tarfile.DIRTYPE
g3.mtime = 1700000000
tf.addfile(g3)
tf.close()
print(_osp.getsize(p) > 0)
f = open(p, "rb")
gz_raw: bytes = cast(bytes, f.read())
f.close()
print(gz_raw[0:2] == b"\x1f\x8b")
z: bytes = gzip.decompress(gz_raw)
print(len(z), z == plain_raw)

with tarfile.open(p, "r:gz") as rd:
    print(rd.getnames())
    print(rd.extractfile("a.txt").read())
    print(rd.extractfile("sub/b.txt").read())
    for m in rd:
        print(m.name, m.type, m.size, m.mtime)

# ── r:* 嗅探明文与 gz；r: 显式明文 ─────────────────────────────
with tarfile.open(pp, "r:*") as r1:
    print(r1.getnames())
with tarfile.open(p, "r:*") as r2:
    print(r2.getnames())
with tarfile.open(pp, "r:") as r3:
    print(r3.getnames())

# ── 外部 gzip 包出来的 tar.gz 也能读（压缩层互操作）─────────────
p2 = d + "/ext.tar.gz"
f = open(p2, "wb")
f.write(cast(str, gzip.compress(plain_raw, 6)))
f.close()
with tarfile.open(p2, "r:gz") as r4:
    print(r4.getnames())
    print(r4.extractfile("a.txt").read())

# ── a 追加 ─────────────────────────────────────────────────────
p3 = d + "/ap.tar"
t3 = tarfile.open(p3, "w")
x = TarInfo("one.txt")
x.size = 3
x.mtime = 1700000000
t3.addfile(x, BytesIO(b"one"))
t3.close()
t4 = tarfile.open(p3, "a")
y = TarInfo("two.txt")
y.size = 3
y.mtime = 1700000000
t4.addfile(y, BytesIO(b"two"))
t4.close()
with tarfile.open(p3) as t5:
    print(t5.getnames())
    print(t5.extractfile("one.txt").read(), t5.extractfile("two.txt").read())
print(_osp.getsize(p3))

# ── x 模式：已存在 -> FileExistsError ───────────────────────────
with tarfile.open(d + "/x.tar", "w") as t6:
    pass
try:
    tarfile.open(d + "/x.tar", "x")
except FileExistsError as e:
    print(type(e).__name__)
t7 = tarfile.open(d + "/new.tar", "x")
z2 = TarInfo("n.txt")
z2.size = 1
z2.mtime = 1700000000
t7.addfile(z2, BytesIO(b"n"))
t7.close()
with tarfile.open(d + "/new.tar") as t8:
    print(t8.getnames())
print(_osp.getsize(d + "/x.tar"), _osp.getsize(d + "/new.tar"))

# ── 错误面 ─────────────────────────────────────────────────────
good = TarInfo("a.txt")
good.size = 0
good.mtime = 1700000000
h = good.tobuf(tarfile.USTAR_FORMAT, "utf-8", "surrogateescape")
bad = h[:148] + b"000000\x00" + h[155:]
pb = d + "/bad.tar"
f = open(pb, "wb")
f.write(cast(str, bad + b"\x00" * 1024))
f.close()
try:
    tarfile.open(pb)
except tarfile.ReadError as e:
    print("badsum", type(e).__name__)

bigmem = BytesIO()
txf = tarfile.open(fileobj=bigmem, mode="w")
big = TarInfo("big.txt")
big.size = 1000
big.mtime = 1700000000
txf.addfile(big, BytesIO(b"x" * 1000))
txf.close()
pt = d + "/trunc.tar"
f = open(pt, "wb")
f.write(cast(str, bigmem.getvalue()[:700]))
f.close()
try:
    with tarfile.open(pt) as r5:
        r5.getmembers()
except tarfile.ReadError as e:
    print("trunc", type(e).__name__)

ptx = d + "/not.tar"
f = open(ptx, "w")
f.write("hello world\n")
f.close()
try:
    tarfile.open(ptx)
except tarfile.ReadError as e:
    print("notar", type(e).__name__)
try:
    tarfile.open(ptx, "r:gz")
except tarfile.ReadError as e:
    print("notgz", type(e).__name__)
try:
    tarfile.open(ptx, "r:zzz")
except tarfile.CompressionError as e:
    print("unk", type(e).__name__)
try:
    tarfile.open(d + "/ap.tar", "r:gz")
except tarfile.ReadError as e:
    print("plainasgz", type(e).__name__)

# ── 空归档 ─────────────────────────────────────────────────────
p9 = d + "/empty.tar"
with tarfile.open(p9, "w") as t9:
    pass
with tarfile.open(p9) as t10:
    print(t10.getnames())
print(_osp.getsize(p9))
# --- expect ---
# 10240
# 10240
# True
# True
# 10240 True
# ['a.txt', 'sub/b.txt', 'sub']
# b'hello'
# b'xyz'
# a.txt b'0' 5 1700000000
# sub/b.txt b'0' 3 1700000001
# sub b'5' 0 1700000000
# ['a.txt', 'sub/b.txt', 'sub']
# ['a.txt', 'sub/b.txt', 'sub']
# ['a.txt', 'sub/b.txt', 'sub']
# ['a.txt', 'sub/b.txt', 'sub']
# b'hello'
# ['one.txt', 'two.txt']
# b'one' b'two'
# 10240
# FileExistsError
# ['n.txt']
# 10240 10240
# badsum ReadError
# trunc ReadError
# notar ReadError
# notgz ReadError
# unk CompressionError
# plainasgz ReadError
# []
# 10240
