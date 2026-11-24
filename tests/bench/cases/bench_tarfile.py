"""tarfile 基准：写 40 个成员 + 读回成员表与内容（真实文件，两边同一 API）。"""
import os
import tarfile
import tempfile

d = tempfile.mkdtemp()
src = os.path.join(d, "src")
os.mkdir(src)
payload = "x" * 60 + "\n"
i = 0
while i < 40:
    f = open(os.path.join(src, "f" + str(i) + ".txt"), "w")
    f.write(payload * (i + 1))
    f.close()
    i += 1

path = os.path.join(d, "b.tar")
total = 0
r = 0
while r < 60:
    tf = tarfile.open(path, "w")
    i = 0
    while i < 40:
        tf.add(os.path.join(src, "f" + str(i) + ".txt"), "f" + str(i) + ".txt")
        i += 1
    tf.close()
    tf = tarfile.open(path, "r")
    members = tf.getmembers()
    total += len(members)
    i = 0
    while i < len(members):
        total += len(tf.extractfile(members[i]).read())
        i += 1
    tf.close()
    r += 1
print(total)
