# pathlib：/ 运算符、str(Path)、iterdir/glob/mkdir
import os
import pathlib

p = pathlib.Path("/tmp")
q = p / "a.txt"
print(q)
print(q.name)
print(q.stem, q.suffix)
print(str(q))
print(q.parent)

root = pathlib.Path("/tmp/py23_pathlib_ops")
root.mkdir(parents=True, exist_ok=True)
# 清掉上次运行残留，保证 iterdir/glob 的期望稳定
for e in root.iterdir():
    if e.is_dir():
        for f in e.iterdir():
            os.remove(str(f))
        os.rmdir(str(e))
    else:
        os.remove(str(e))

(root / "a.txt").write_text("a")
(root / "b.txt").write_text("b")
(root / "c.log").write_text("c")
sub = root / "sub"
sub.mkdir()
(sub / "d.txt").write_text("d")

names = []
for e in root.iterdir():
    names.append(e.name)
names.sort()
print(names)

txt = []
for e in root.glob("*.txt"):
    txt.append(e.name)
txt.sort()
print(txt)

rec = []
for e in root.glob("**/*.txt"):
    rec.append(e.name)
rec.sort()
print(rec)

print((root / "a.txt").read_text())
root.mkdir(exist_ok=True)
sub.mkdir(exist_ok=True)
print("ok")
# --- expect ---
# /tmp/a.txt
# a.txt
# a .txt
# /tmp/a.txt
# /tmp
# ['a.txt', 'b.txt', 'c.log', 'sub']
# ['a.txt', 'b.txt']
# ['a.txt', 'b.txt', 'd.txt']
# a
# ok
