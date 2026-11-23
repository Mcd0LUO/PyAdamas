# pathlib.Path 声明式签名迁移：构造器/方法/属性与 / 运算符对拍 CPython。
import os
from pathlib import Path

root = Path("/tmp/py23_pathlib_560")
root.mkdir(parents=True, exist_ok=True)
for e in root.iterdir():
    if e.is_dir():
        for f in e.iterdir():
            os.remove(str(f))
        os.rmdir(str(e))
    else:
        os.remove(str(e))

p = root / "a.txt"
print(str(p), p.name, p.stem, p.suffix)
print(str(p.parent) == str(root))
print(p.is_absolute())

p.write_text("hello")
print(p.exists(), p.is_file(), p.is_dir())
print(p.read_text())

p.write_bytes(b"xyz")
print(p.read_bytes())

q = p.with_suffix(".log")
print(q.name, q.suffix)
print(p.joinpath("sub").name)
print(str(p.relative_to(str(root))))

sub = root / "sub"
sub.mkdir()
print(sub.is_dir())
(sub / "b.txt").write_text("b")
print((sub / "b.txt").read_text())
print(sub.joinpath("c.txt").name)

names = []
for e in root.iterdir():
    names.append(e.name)
names.sort()
print(names)

globs = []
for e in root.glob("*.txt"):
    globs.append(e.name)
globs.sort()
print(globs)

rec = []
for e in root.glob("**/*.txt"):
    rec.append(e.name)
rec.sort()
print(rec)

root.mkdir(exist_ok=True)
sub.mkdir(exist_ok=True)
print(root.is_absolute(), (root / "none").exists())

(root / "b.txt").touch()
print((root / "b.txt").exists())
(root / "b.txt").unlink()
print((root / "b.txt").exists())

os.remove(str(sub / "b.txt"))
sub.rmdir()
p.unlink()
root.rmdir()
print("done")
# --- expect ---
# /tmp/py23_pathlib_560/a.txt a.txt a .txt
# True
# True
# True True False
# hello
# b'xyz'
# a.log .log
# sub
# a.txt
# True
# b
# c.txt
# ['a.txt', 'sub']
# ['a.txt']
# ['a.txt', 'b.txt']
# True False
# True
# False
# done
