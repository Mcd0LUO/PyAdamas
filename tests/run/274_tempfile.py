# 274 tempfile：gettempdir/gettempprefix、mkdtemp/mkstemp（前缀/后缀/dir、
# fd 经 open(fd) 包装、os.close 裸 fd）、NamedTemporaryFile（w+b/w+/r+b、
# delete=False、readline/readlines/迭代）、TemporaryDirectory（cleanup 幂等）、
# 错误类型。随机名只钉属性不钉值。对拍 CPython 3.14。
import tempfile
import os
import os.path as osp
import shutil
from typing import cast

print("prefix:", tempfile.gettempprefix())
td = tempfile.gettempdir()
print("tempdir usable:", osp.isdir(td))
print("cache:", tempfile.gettempdir() == td)

d1 = tempfile.mkdtemp()
print("d1 default:", osp.basename(d1).startswith("tmp"), osp.isdir(d1))
d2 = tempfile.mkdtemp(suffix=".s", prefix="p_", dir=d1)
print("d2 nested:", osp.dirname(d2) == d1, osp.basename(d2).startswith("p_"), osp.basename(d2).endswith(".s"))
d3 = tempfile.mkdtemp(suffix=None, prefix=None, dir=None)
print("d3 none:", osp.basename(d3).startswith("tmp"))
os.rmdir(d2)
os.rmdir(d1)
os.rmdir(d3)

fd, p = tempfile.mkstemp()
print("mkstemp default:", osp.basename(p).startswith("tmp"), p.startswith("/"))
f = open(fd, "w")
f.write("data")
f.close()
g = open(p, "rb")
gb: bytes = cast(bytes, g.read())
print("mkstemp content:", gb)
g.close()
os.unlink(p)
fd2, p2 = tempfile.mkstemp(suffix=".log", prefix="pre", dir=td)
print("mkstemp attrs:", osp.basename(p2).startswith("pre"), osp.basename(p2).endswith(".log"))
os.close(fd2)
os.unlink(p2)

with tempfile.NamedTemporaryFile() as t:
    t.write(cast(str, b"abc"))
    t.seek(0)
    rb: bytes = cast(bytes, t.read())
    print("ntf wb:", rb, t.readable(), t.writable(), t.seekable())
    print("ntf tell:", t.tell())
    nm = t.name
print("ntf deleted:", not osp.exists(nm))

with tempfile.NamedTemporaryFile(mode="w+", delete=False, suffix=".txt", prefix="ntf_") as t:
    t.write("text\nmode")
    t.flush()
    print("ntf text exists:", osp.exists(t.name))
    t.seek(0)
    print("ntf text read:", t.read())
    print("ntf name attrs:", osp.basename(t.name).startswith("ntf_"), osp.basename(t.name).endswith(".txt"))
    nm2 = t.name
print("ntf kept:", osp.exists(nm2))
os.unlink(nm2)

with tempfile.NamedTemporaryFile(mode="r+b") as t:
    t.write(cast(str, b"12345"))
    t.seek(1)
    t.write(cast(str, b"XY"))
    t.seek(0)
    rb2: bytes = cast(bytes, t.read())
    print("ntf r+b:", rb2)

with tempfile.NamedTemporaryFile(mode="w+") as t:
    t.write("l1\nl2\nl3")
    t.seek(0)
    print("ntf readline:", t.readline())
    print("ntf readlines:", t.readlines())
    t.seek(0)
    lines: list[str] = []
    for ln in t:
        lines.append(ln)
    print("ntf iter:", lines)

with tempfile.TemporaryDirectory() as tdir:
    print("td isdir:", osp.isdir(tdir), osp.basename(tdir).startswith("tmp"))
    fw = open(osp.join(tdir, "a.txt"), "wb")
    fw.write(cast(str, b"x"))
    fw.close()
    sub = tempfile.mkdtemp(dir=tdir)
    print("td nested:", osp.dirname(sub) == tdir)
print("td cleaned:", not osp.exists(tdir))

td2 = tempfile.TemporaryDirectory(suffix=".d", prefix="tdp_")
print("td2 attrs:", osp.basename(td2.name).startswith("tdp_"), osp.basename(td2.name).endswith(".d"))
td2.cleanup()
print("td2 cleaned:", not osp.exists(td2.name))
td2.cleanup()
print("td2 double cleanup ok")

try:
    tempfile.mkdtemp(dir="/no_such_xyz")
except FileNotFoundError:
    print("mkdtemp FNF ok")
try:
    tempfile.mkstemp(dir="/no_such_xyz")
except FileNotFoundError:
    print("mkstemp FNF ok")
try:
    tempfile.NamedTemporaryFile(dir="/no_such_xyz")
except FileNotFoundError:
    print("ntf FNF ok")
try:
    tempfile.TemporaryDirectory(dir="/no_such_xyz")
except FileNotFoundError:
    print("td FNF ok")
print("done")

# --- expect ---
# prefix: tmp
# tempdir usable: True
# cache: True
# d1 default: True True
# d2 nested: True True True
# d3 none: True
# mkstemp default: True True
# mkstemp content: b'data'
# mkstemp attrs: True True
# ntf wb: b'abc' True True True
# ntf tell: 3
# ntf deleted: True
# ntf text exists: True
# ntf text read: text
# mode
# ntf name attrs: True True
# ntf kept: True
# ntf r+b: b'1XY45'
# ntf readline: l1
# 
# ntf readlines: ['l2\n', 'l3']
# ntf iter: ['l1\n', 'l2\n', 'l3']
# td isdir: True True
# td nested: True
# td cleaned: True
# td2 attrs: True True
# td2 cleaned: True
# td2 double cleanup ok
# mkdtemp FNF ok
# mkstemp FNF ok
# ntf FNF ok
# td FNF ok
# done
