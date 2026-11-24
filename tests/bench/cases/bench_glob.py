"""glob 基准：固定小目录树上的通配展开（含 escape 与 ** 递归）。"""
import glob
import os
import tempfile

root = tempfile.mkdtemp()
i = 0
while i < 12:
    d = os.path.join(root, "d" + str(i))
    os.mkdir(d)
    j = 0
    while j < 12:
        f = open(os.path.join(d, "f" + str(j) + ".txt"), "w")
        f.write("x")
        f.close()
        j += 1
    i += 1

cwd = os.getcwd()
os.chdir(root)
total = 0
r = 0
while r < 60:
    total += len(glob.glob("d*/*.txt"))
    total += len(glob.glob("d1?/f[0-5].txt"))
    total += len(glob.glob("**/*.txt", recursive=True))
    total += len(glob.glob("nomatch*"))
    total += len(glob.escape("a*b?c[d]"))
    r += 1
os.chdir(cwd)
print(total)
