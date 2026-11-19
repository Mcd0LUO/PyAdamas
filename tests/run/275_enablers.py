# 275 stdlib2 使能回归：
# F1 bytes 切片 eq/ne 在循环条件里（cmp+branch 融合曾按指针比较，基线 bug）；
# E1 open() errno → FileNotFoundError/FileExistsError/IsADirectoryError/NotADirectoryError；
# E2 os.close/os.unlink；E3 os.makedirs exist_ok 关键字；E4 os.path.islink。
import os
import os.path as osp
import tempfile
import shutil
import subprocess
from typing import cast

# F1：bytes 切片比较在 while/for 条件位置
data = b"PK\x01\x02rest"
pos = 0
i = 0
while i < 1:
    if data[pos:pos + 4] == b"PK\x01\x02":
        print("while eq: match")
    else:
        print("while eq: MISMATCH")
    if data[pos:pos + 4] != b"PK\x01\x02":
        print("while ne: MISMATCH")
    else:
        print("while ne: match")
    i += 1
for j in range(1):
    if data[pos:pos + 4] != b"PK\x01\x02":
        print("for ne: MISMATCH")
    else:
        print("for ne: match")
whole = b"abc"
i = 0
while i < 1:
    print("whole eq:", whole == b"abc", whole != b"abd")
    i += 1

d = tempfile.mkdtemp(prefix="pa275_")
try:
    # E1：open errno 映射
    try:
        open(d + "/no_such_dir/a.txt", "rb")
    except FileNotFoundError:
        print("open FNF ok")
    try:
        open(d + "/no_such_dir/a.txt", "wb")
    except FileNotFoundError:
        print("open wb FNF ok")
    fw = open(d + "/f.txt", "wb")
    fw.write(cast(str, b"x"))
    fw.close()
    try:
        open(d + "/f.txt", "xb")
    except FileExistsError:
        print("open FEX ok")
    try:
        open(d, "wb")
    except IsADirectoryError:
        print("open IAD ok")
    try:
        open(d + "/f.txt/child", "rb")
    except NotADirectoryError:
        print("open NAD ok")

    # E2：os.unlink / os.close
    os.unlink(d + "/f.txt")
    print("unlink:", not osp.exists(d + "/f.txt"))
    fw = open(d + "/g.txt", "wb")
    fd = fw.fileno()
    fw2 = open(fd, "wb", closefd=False)
    fw2.write(cast(str, b"hi"))
    fw2.close()
    fw.close()
    fr = open(d + "/g.txt", "rb")
    gb: bytes = cast(bytes, fr.read())
    print("dup fd:", gb)
    fr.close()
    os.unlink(d + "/g.txt")

    # E3：makedirs exist_ok 关键字
    os.makedirs(d + "/m1/m2")
    os.makedirs(d + "/m1/m2", exist_ok=True)
    print("makedirs kw ok")
    try:
        os.makedirs(d + "/m1/m2", exist_ok=False)
    except FileExistsError:
        print("makedirs FEX ok")

    # E4：os.path.islink
    subprocess.run(["ln", "-s", "m1", d + "/lnk"], capture_output=True)
    print("islink:", osp.islink(d + "/lnk"), osp.islink(d + "/m1"), osp.islink(d + "/nope"))
    print("done")
finally:
    shutil.rmtree(d, ignore_errors=True)

# --- expect ---
# while eq: match
# while ne: match
# for ne: match
# whole eq: True True
# open FNF ok
# open wb FNF ok
# open FEX ok
# open IAD ok
# open NAD ok
# unlink: True
# dup fd: b'hi'
# makedirs kw ok
# makedirs FEX ok
# islink: True False False
# done
