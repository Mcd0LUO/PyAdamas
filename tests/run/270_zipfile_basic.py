# 270 zipfile 基础面：writestr/read（stored+deflated）、namelist/infolist/getinfo、
# date_time/comment、testzip、is_zipfile、extract/extractall（路径净化）、
# append/x 模式、write() 落盘文件、ZipFile.open r/w。
# 对拍 CPython 3.14（/tmp/zdiff.py harness 同源），O0/O1/O2 一致。
import zipfile
import zlib
import os
import os.path as osp
import tempfile
import shutil
import binascii
from typing import cast

d = tempfile.mkdtemp(prefix="pa270_")
try:
    p = d + "/t.zip"
    with zipfile.ZipFile(p, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("a.txt", "hello world\nline2")
        z.writestr("dir/b.bin", b"\x00\x01\x02\xff")
        z.writestr("stored.txt", "s" * 500, compress_type=zipfile.ZIP_STORED)
        z.writestr("empty.txt", b"")
        z.writestr("adir/", b"")
        zdated = zipfile.ZipInfo("dated.txt", (2021, 2, 3, 4, 5, 6))
        z.writestr(zdated, "dated content")
        z.comment = b"batch2"
    with zipfile.ZipFile(p) as z:
        print(z.namelist())
        print(z.read("a.txt"))
        print(binascii.hexlify(z.read("dir/b.bin")))
        print(z.read("empty.txt"))
        ai = z.getinfo("a.txt")
        print(ai.file_size, ai.compress_type, ai.is_dir())
        print(z.getinfo("dated.txt").date_time)
        si = z.getinfo("stored.txt")
        print(si.compress_type, si.file_size, si.compress_size)
        print(z.getinfo("adir/").is_dir())
        print(z.testzip() is None)
        print(z.comment)
        print(len(z.infolist()))
        print(z.read("stored.txt") == b"s" * 500)
        zi = zipfile.ZipInfo("zi.txt", (2024, 5, 6, 7, 8, 9))
        print(zi.date_time, zi.compress_type, zi.is_dir())
        print(zipfile.ZipInfo("dd/").is_dir())
        z.extractall(d + "/ex")
    print(osp.exists(d + "/ex/a.txt"))
    f = open(d + "/ex/a.txt", "rb")
    fb: bytes = cast(bytes, f.read())
    print(fb)
    f.close()
    print(osp.exists(d + "/ex/dir/b.bin"))
    print(zipfile.is_zipfile(p), zipfile.is_zipfile(d + "/ex/a.txt"))

    # extract 路径净化（../ 与绝对路径）
    p2 = d + "/evil.zip"
    with zipfile.ZipFile(p2, "w") as z:
        z.writestr("../evil.txt", "sanitized")
        z.writestr("/abs/path.txt", "abs")
        z.writestr("a/../../b.txt", "dots")
    with zipfile.ZipFile(p2) as z:
        z.extractall(d + "/ex2")
        print(z.extract("a/../../b.txt", d + "/ex3") == d + "/ex3/a/b.txt")
    print(osp.exists(d + "/ex2/evil.txt"), osp.exists(d + "/evil.txt"))
    print(osp.exists(d + "/ex2/abs/path.txt"))
    print(osp.exists(d + "/ex2/b.txt"), osp.exists(d + "/ex3/b.txt"))

    # append 模式
    p3 = d + "/app.zip"
    with zipfile.ZipFile(p3, "w") as z:
        z.writestr("one.txt", "first")
    with zipfile.ZipFile(p3, "a") as z:
        z.writestr("two.txt", "second")
        print(z.read("one.txt"))
    with zipfile.ZipFile(p3) as z:
        print(z.namelist())
        print(z.read("two.txt"))

    # x 模式
    p4 = d + "/x.zip"
    with zipfile.ZipFile(p4, "x") as z:
        z.writestr("x.txt", "excl")
    try:
        zipfile.ZipFile(p4, "x")
    except FileExistsError:
        print("x FEX ok")

    # write() 从真实文件 + arcname
    src = d + "/src.bin"
    fw = open(src, "wb")
    fw.write(cast(str, b"file-content\x00\xff"))
    fw.close()
    p5 = d + "/w.zip"
    with zipfile.ZipFile(p5, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(src, "renamed.bin")
        z.write(src)
    with zipfile.ZipFile(p5) as z:
        nl = z.namelist()
        print(nl[0], nl[1].endswith("/src.bin"))
        print(binascii.hexlify(z.read("renamed.bin")))
        print(z.getinfo("renamed.bin").file_size)

    # ZipFile.open r/w
    p6 = d + "/zopen.zip"
    with zipfile.ZipFile(p6, "w", zipfile.ZIP_DEFLATED) as z:
        with z.open("wf.txt", "w") as fw2:
            fw2.write(b"open-write " * 20)
    with zipfile.ZipFile(p6) as z:
        with z.open("wf.txt") as fr:
            print(fr.tell())
            print(fr.read(10))
            print(fr.readline())
            fr.seek(0)
            print(len(fr.read()))
            fr.seek(-5, 2)
            print(fr.read())
        print(z.testzip() is None)
    print("done")
finally:
    shutil.rmtree(d, ignore_errors=True)

# --- expect ---
# ['a.txt', 'dir/b.bin', 'stored.txt', 'empty.txt', 'adir/', 'dated.txt']
# b'hello world\nline2'
# b'000102ff'
# b''
# 17 8 False
# (2021, 2, 3, 4, 5, 6)
# 0 500 500
# True
# True
# b'batch2'
# 6
# True
# (2024, 5, 6, 7, 8, 9) 0 False
# True
# True
# b'hello world\nline2'
# True
# True False
# True
# True False
# True
# False False
# b'first'
# ['one.txt', 'two.txt']
# b'second'
# x FEX ok
# renamed.bin True
# b'66696c652d636f6e74656e7400ff'
# 14
# 0
# b'open-write'
# b' open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write open-write '
# 220
# b'rite '
# True
# done
