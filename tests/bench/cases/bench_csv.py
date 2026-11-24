"""csv 基准：writer 回写 200 行 + reader 解析（临时文件，两边同一份 API）。"""
import csv
import os
import tempfile

path = os.path.join(tempfile.mkdtemp(), "bench.csv")

total = 0
r = 0
while r < 120:
    f = open(path, "w")
    w = csv.writer(f)
    w.writerow(["id", "name", "note", "num"])
    i = 0
    while i < 200:
        w.writerow([i, "name" + str(i), "quoted, x", 42])
        i += 1
    f.close()
    rd = open(path, "r")
    n = 0
    for row in csv.reader(rd):
        n += 1
    rd.close()
    total += n
    r += 1
print(total)
