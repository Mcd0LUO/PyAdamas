# 272 shutil 复制面：copyfile/copy/copy2/copymode/copystat（mode+mtime）、
# 覆盖写、目录目标、which、SameFileError/FileNotFoundError/IsADirectoryError。
# mode 用 subprocess stat -c 读（两边同口径）；对拍 CPython 3.14。
import shutil
import os
import os.path as osp
import subprocess
import tempfile
from typing import cast

d = tempfile.mkdtemp(prefix="pa272_")
try:
    os.makedirs(d + "/src", exist_ok=True)
    os.makedirs(d + "/dstd", exist_ok=True)

    def wf(path: str, data: bytes) -> None:
        f = open(path, "wb")
        f.write(cast(str, data))
        f.close()

    def rf(path: str) -> bytes:
        f = open(path, "rb")
        b: bytes = cast(bytes, f.read())
        f.close()
        return b

    wf(d + "/src/a.txt", b"alpha\n")
    wf(d + "/src/big.bin", bytes([i % 251 for i in range(200000)]))
    os.chmod(d + "/src/a.txt", 0o640)

    shutil.copyfile(d + "/src/a.txt", d + "/c1.txt")
    print("c1:", rf(d + "/c1.txt"))
    wf(d + "/c1.txt", b"old-longer-content")
    shutil.copyfile(d + "/src/a.txt", d + "/c1.txt")
    print("overwrite:", rf(d + "/c1.txt"))
    shutil.copyfile(d + "/src/big.bin", d + "/big_copy.bin")
    print("big:", rf(d + "/big_copy.bin") == rf(d + "/src/big.bin"))

    r = shutil.copy(d + "/src/a.txt", d + "/dstd")
    print("copy dst:", r == d + "/dstd/a.txt", rf(r))
    r2 = shutil.copy2(d + "/src/a.txt", d + "/dstd")
    print("copy2 dst:", r2 == d + "/dstd/a.txt")

    mt = osp.getmtime(d + "/src/a.txt")
    shutil.copy2(d + "/src/a.txt", d + "/c2.txt")
    print("mtime:", osp.getmtime(d + "/c2.txt") == mt)
    sm = subprocess.run(["stat", "-c", "%a", d + "/c2.txt"], capture_output=True, text=True)
    print("mode:", sm.stdout.strip())

    wf(d + "/c3.txt", b"xxx")
    shutil.copystat(d + "/src/a.txt", d + "/c3.txt")
    print("copystat mtime:", osp.getmtime(d + "/c3.txt") == mt)
    print("copystat content:", rf(d + "/c3.txt"))
    shutil.copymode(d + "/src/a.txt", d + "/c3.txt")
    sm2 = subprocess.run(["stat", "-c", "%a", d + "/c3.txt"], capture_output=True, text=True)
    print("copymode:", sm2.stdout.strip())

    try:
        shutil.copyfile(d + "/nope", d + "/x")
    except FileNotFoundError:
        print("copyfile FNF ok")
    try:
        shutil.copyfile(d + "/src/a.txt", d + "/src/a.txt")
    except shutil.SameFileError:
        print("SameFile ok")
    try:
        shutil.copyfile(d + "/src/a.txt", d + "/dstd")
    except IsADirectoryError:
        print("copyfile IAD ok")
    try:
        shutil.copy2(d + "/nope", d + "/x")
    except FileNotFoundError:
        print("copy2 FNF ok")

    print("which sh:", shutil.which("sh") is not None)
    print("which missing:", shutil.which("no_such_cmd_xyz") is None)
    print("which path:", shutil.which("sh", path=d + "/nope") is None)
    wf(d + "/mytool", b"#!/bin/sh\n")
    os.chmod(d + "/mytool", 0o755)
    print("which custom:", shutil.which("mytool", path=d) == d + "/mytool")
    print("done")
finally:
    shutil.rmtree(d, ignore_errors=True)

# --- expect ---
# c1: b'alpha\n'
# overwrite: b'alpha\n'
# big: True
# copy dst: True b'alpha\n'
# copy2 dst: True
# mtime: True
# mode: 640
# copystat mtime: True
# copystat content: b'xxx'
# copymode: 640
# copyfile FNF ok
# SameFile ok
# copyfile IAD ok
# copy2 FNF ok
# which sh: True
# which missing: True
# which path: True
# which custom: True
# done
