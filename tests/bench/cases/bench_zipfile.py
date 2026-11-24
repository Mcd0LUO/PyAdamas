"""zipfile 基准：写 40 个成员（deflated）+ 读回。"""
import os
import tempfile
import zipfile

path = os.path.join(tempfile.mkdtemp(), "b.zip")
payload = "x" * 60 + "\n"

total = 0
r = 0
while r < 60:
    z = zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED)
    i = 0
    while i < 40:
        z.writestr("dir/file" + str(i) + ".txt", payload * (i + 1))
        i += 1
    z.close()
    z = zipfile.ZipFile(path, "r")
    names = z.namelist()
    total += len(names)
    i = 0
    while i < len(names):
        total += len(z.read(names[i]))
        i += 1
    z.close()
    r += 1
print(total)
