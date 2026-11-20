# 362 tarfile 的磁盘面：add（递归目录/arcname）、gettarinfo（lstat 面）、
# extract/extractall（含 members 子集、目录属性延迟、安全名净化）、
# 符号链接解压与再次入档、硬链接成员回退成复制、mtime/mode 保留。
# 对拍 CPython 3.14：输出逐字节一致（uid/gid/uname 不打印，见模块文档）。
# pyadamas: lang-gaps
import os
import os.path as _osp
import tempfile
import tarfile
from io import BytesIO
from tarfile import TarInfo


d = tempfile.mkdtemp()
os.makedirs(d + "/src/sub")
f = open(d + "/src/a.txt", "w")
f.write("alpha\n")
f.close()
os.chmod(d + "/src/a.txt", 0o640)
f = open(d + "/src/sub/b.bin", "w")
f.write("\x01\x02\x03")
f.close()
os.chmod(d + "/src/sub/b.bin", 0o600)
os.chmod(d + "/src/sub", 0o750)
os.chmod(d + "/src", 0o750)

p = d + "/a.tar"
with tarfile.open(p, "w") as tf:
    tf.add(d + "/src", arcname="src")

with tarfile.open(p) as rd:
    print(rd.getnames())
    for m in rd:
        print(m.name, m.type, m.size, m.mode, m.isfile(), m.isdir())

# ── gettarinfo 面 ───────────────────────────────────────────────
with tarfile.open(d + "/b.tar", "w") as tf2:
    ti = tf2.gettarinfo(d + "/src/a.txt", "renamed.txt")
    print(ti.name, ti.size, ti.type, ti.mode, ti.linkname)
    di = tf2.gettarinfo(d + "/src", "sd")
    print(di.name, di.size, di.type, di.isdir())
    try:
        tf2.gettarinfo(d + "/nope", "nope")
    except FileNotFoundError as e:
        print(type(e).__name__)

# ── extractall / extract / members 子集 ─────────────────────────
out1 = d + "/out1"
with tarfile.open(p) as rd2:
    rd2.extractall(out1)
print(_osp.isdir(out1 + "/src"), _osp.isdir(out1 + "/src/sub"))
g = open(out1 + "/src/a.txt", "r")
print("src/a.txt", repr(g.read()))
g.close()
g = open(out1 + "/src/sub/b.bin", "r")
bs: str = g.read()
print("src/sub/b.bin", len(bs), ord(bs[0]), ord(bs[2]))
g.close()
print(_osp.getsize(out1 + "/src/a.txt"), _osp.getsize(out1 + "/src/sub/b.bin"))

out2 = d + "/out2"
with tarfile.open(p) as rd3:
    rd3.extractall(out2, [rd3.getmember("src/sub/b.bin")])
print(_osp.exists(out2 + "/src/sub/b.bin"), _osp.exists(out2 + "/src/a.txt"))

out3 = d + "/out3"
with tarfile.open(p) as rd4:
    rd4.extract("src/a.txt", out3)
print(_osp.exists(out3 + "/src/a.txt"), _osp.exists(out3 + "/src/sub/b.bin"))

# ── 确定性 mtime：手写 TarInfo + extract，检查 utime ──────────────
b = BytesIO()
tf3 = tarfile.open(fileobj=b, mode="w")
t = TarInfo("mt.txt")
t.size = 3
t.mode = 0o100640
t.mtime = 1700000000
tf3.addfile(t, BytesIO(b"mt!"))
tf3.close()
b.seek(0)
out4 = d + "/out4"
with tarfile.open(fileobj=b, mode="r") as rd5:
    rd5.extractall(out4)
print(_osp.getmtime(out4 + "/mt.txt") == 1700000000)

# ── 符号链接：手写成员 -> 解压建链 -> 链再入档 -> 再解压 ──────────
b2 = BytesIO()
tf4 = tarfile.open(fileobj=b2, mode="w")
tg = TarInfo("target.txt")
tg.size = 6
tg.mtime = 1700000000
tf4.addfile(tg, BytesIO(b"target"))
sl = TarInfo("link")
sl.type = tarfile.SYMTYPE
sl.linkname = "target.txt"
sl.mtime = 1700000000
tf4.addfile(sl)
tf4.close()
b2.seek(0)
d1 = d + "/d1"
with tarfile.open(fileobj=b2, mode="r") as rd6:
    rd6.extractall(d1)
print(_osp.islink(d1 + "/link"))
g = open(d1 + "/link", "r")
print(repr(g.read()))
g.close()

p2 = d + "/t2.tar"
with tarfile.open(p2, "w") as tf5:
    tf5.add(d1 + "/link", "link2")
    tf5.add(d1 + "/target.txt", "target.txt")
with tarfile.open(p2) as rd7:
    for m in rd7:
        print(m.name, m.type, m.linkname, m.size)
d2 = d + "/d2"
with tarfile.open(p2) as rd8:
    rd8.extractall(d2)
print(_osp.islink(d2 + "/link2"))
g = open(d2 + "/link2", "r")
print(repr(g.read()))
g.close()

# ── 硬链接（LNKTYPE）成员：pa 回退成复制，内容一致 ────────────────
b3 = BytesIO()
tf6 = tarfile.open(fileobj=b3, mode="w")
ht = TarInfo("hard_target.txt")
ht.size = 4
ht.mtime = 1700000000
tf6.addfile(ht, BytesIO(b"hard"))
hl = TarInfo("hardlink")
hl.type = tarfile.LNKTYPE
hl.linkname = "hard_target.txt"
hl.mtime = 1700000000
tf6.addfile(hl)
tf6.close()
b3.seek(0)
with tarfile.open(fileobj=b3, mode="r") as rd9:
    rd9.extractall(d + "/d3")
g = open(d + "/d3/hardlink", "r")
print(repr(g.read()))
g.close()
print(_osp.getsize(d + "/d3/hardlink"))

# ── 安全名净化：绝对路径/上跳成员落在解压根下 ──────────────────
b4 = BytesIO()
tf7 = tarfile.open(fileobj=b4, mode="w")
ax = TarInfo("/abs.txt")
ax.size = 2
ax.mtime = 1700000000
tf7.addfile(ax, BytesIO(b"ab"))
tf7.close()
b4.seek(0)
d4 = d + "/d4"
with tarfile.open(fileobj=b4, mode="r") as rd10:
    rd10.extractall(d4)
print(_osp.exists(d4 + "/abs.txt"))
# --- expect ---
# ['src', 'src/a.txt', 'src/sub', 'src/sub/b.bin']
# src b'5' 0 488 False True
# src/a.txt b'0' 6 416 True False
# src/sub b'5' 0 488 False True
# src/sub/b.bin b'0' 3 384 True False
# renamed.txt 6 b'0' 33184 
# sd 0 b'5' True
# FileNotFoundError
# True True
# src/a.txt 'alpha\n'
# src/sub/b.bin 3 1 3
# 6 3
# True False
# True False
# True
# True
# 'target'
# link2 b'2' target.txt 0
# target.txt b'0'  6
# True
# 'target'
# 'hard'
# 4
# True
