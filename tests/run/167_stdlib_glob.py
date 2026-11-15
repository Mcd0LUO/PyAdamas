"""stdlib/glob.py：* ? [] 与 recursive ** 的匹配，escape/iglob。"""
import glob
import os

root = "/tmp/pa_glob_test_" + str(os.getpid())
os.mkdir(root)
os.mkdir(root + "/sub")


def touch(path: str) -> None:
    f = open(path, "w")
    f.write("x")
    f.close()


def show(pattern: str, recursive: bool = False) -> None:
    out: list[str] = []
    for p in glob.glob(pattern, recursive):
        out.append(p[len(root) + 1:])
    print(sorted(out))


touch(root + "/a.txt")
touch(root + "/b.txt")
touch(root + "/c.log")
touch(root + "/sub/d.txt")
show(root + "/*.txt")
show(root + "/?.txt")
show(root + "/[ab].txt")
show(root + "/sub/*.txt")
show(root + "/nope*")
show(root + "/**/*.txt", True)
print(glob.escape("a*b"), len(glob.iglob(root + "/*.txt")))
os.remove(root + "/a.txt")
os.remove(root + "/b.txt")
os.remove(root + "/c.log")
os.remove(root + "/sub/d.txt")
os.rmdir(root + "/sub")
os.rmdir(root)

# --- expect ---
# ['a.txt', 'b.txt']
# ['a.txt', 'b.txt']
# ['a.txt', 'b.txt']
# ['sub/d.txt']
# []
# ['a.txt', 'b.txt', 'sub/d.txt']
# a[*]b 2
