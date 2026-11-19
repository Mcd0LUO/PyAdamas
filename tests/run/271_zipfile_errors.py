# 271 zipfile 错误路径 + 互操作 fixture：BadZipFile/KeyError/FileNotFoundError/
# ValueError/NotImplementedError（bzip2/lzma/zip64）/RuntimeError（加密）、
# CRC 损坏、截断、data-descriptor（CPython 流式产物）读取。
# fixture 由 CPython 3.14 离线生成（base64 内嵌），两边读同一份字节。
import zipfile
import os
import os.path as osp
import tempfile
import shutil
import base64
from typing import cast

# CPython 流式写（non-seekable）产物：local header flag bit 3（data descriptor）
DESC_B64 = "UEsDBBQACAAIAAAAIQAAAAAAAAAAAAAAAAAKAAAAc3RyZWFtLnR4dCsuKUpNzE1NUUhJLElUKB7ljfJGeaO8UR6NeQBQSwcIT8Xf1RwAAAB4BQAAUEsDBBQACAAIAAYLdF0AAAAAAAAAAAAAAAAJAAAAcGxhaW4udHh0K8hJzMwDAFBLBwjPYiAZBwAAAAUAAABQSwECFAMUAAgACAAAACEAT8Xf1RwAAAB4BQAACgAAAAAAAAAAAAAAgAEAAAAAc3RyZWFtLnR4dFBLAQIUAxQACAAIAAYLdF3PYiAZBwAAAAUAAAAJAAAAAAAAAAAAAACAAVQAAABwbGFpbi50eHRQSwUGAAAAAAIAAgBvAAAAkgAAAAAA"
# CPython force_zip64 产物（zip64 extra field）
ZIP64_B64 = "UEsDBC0AAAAAAAAAIQBqtulI//////////8HABQAejY0LnR4dAEAEAANAAAAAAAAAA0AAAAAAAAAemlwNjQgY29udGVudFBLAQItAy0AAAAAAAAAIQBqtulIDQAAAA0AAAAHAAAAAAAAAAAAAACAAQAAAAB6NjQudHh0UEsFBgAAAAABAAEANQAAAEYAAAAAAA=="
# 加密标志位置位的产物（flag bit 0）
ENC_B64 = "UEsDBBQAAQAAAJEUdF2DFtyMAQAAAAEAAAAFAAAAZS50eHR4UEsBAhQDFAABAAAAkRR0XYMW3IwBAAAAAQAAAAUAAAAAAAAAAAAAAIABAAAAAGUudHh0UEsFBgAAAAABAAEAMwAAACQAAAAAAA=="

d = tempfile.mkdtemp(prefix="pa271_")
try:
    # data descriptor 读取
    p = d + "/desc.zip"
    fw = open(p, "wb")
    fw.write(cast(str, base64.b64decode(DESC_B64.encode())))
    fw.close()
    with zipfile.ZipFile(p) as z:
        print(z.getinfo("stream.txt").flag_bits & 8)
        print(len(z.read("stream.txt")))
        print(z.read("plain.txt"))
        print(z.testzip() is None)

    # zip64 → NotImplementedError（文档化边界）
    p64 = d + "/z64.zip"
    fw = open(p64, "wb")
    fw.write(cast(str, base64.b64decode(ZIP64_B64.encode())))
    fw.close()
    try:
        zipfile.ZipFile(p64)
    except NotImplementedError:
        print("zip64 NI ok")

    # 加密 → RuntimeError（与 CPython 无密码同型）
    pe = d + "/enc.zip"
    fw = open(pe, "wb")
    fw.write(cast(str, base64.b64decode(ENC_B64.encode())))
    fw.close()
    with zipfile.ZipFile(pe) as z:
        try:
            z.read("e.txt")
        except RuntimeError:
            print("encrypted RE ok")

    # 不存在 / 非 zip / 截断
    try:
        zipfile.ZipFile(d + "/nope.zip")
    except FileNotFoundError:
        print("FNF ok")
    pn = d + "/notzip.bin"
    fw = open(pn, "wb")
    fw.write(cast(str, b"this is not a zip file at all"))
    fw.close()
    try:
        zipfile.ZipFile(pn)
    except zipfile.BadZipFile:
        print("badzip ok")
    print(zipfile.is_zipfile(pn))

    # CRC 损坏
    pc = d + "/crc.zip"
    with zipfile.ZipFile(pc, "w") as z:
        z.writestr("victim.txt", "important data")
    fr = open(pc, "rb")
    raw: bytes = cast(bytes, fr.read())
    fr.close()
    patched = raw[:40] + bytes([raw[40] ^ 0xFF]) + raw[41:]
    fw = open(pc, "wb")
    fw.write(cast(str, patched))
    fw.close()
    with zipfile.ZipFile(pc) as z:
        try:
            z.read("victim.txt")
        except zipfile.BadZipFile:
            print("crc BadZip ok")
        print(z.testzip())
    pt = d + "/trunc.zip"
    fw = open(pt, "wb")
    fw.write(cast(str, raw[:30]))
    fw.close()
    try:
        zipfile.ZipFile(pt)
    except zipfile.BadZipFile:
        print("trunc ok")

    # getinfo/read 缺失名、关闭后操作、非法模式、只写读
    pg = d + "/ok.zip"
    with zipfile.ZipFile(pg, "w") as z:
        z.writestr("a.txt", "x")
    with zipfile.ZipFile(pg) as z:
        try:
            z.getinfo("missing.txt")
        except KeyError:
            print("KeyError ok")
        try:
            z.read("missing.txt")
        except KeyError:
            print("read KeyError ok")
    z = zipfile.ZipFile(pg)
    z.close()
    try:
        z.read("a.txt")
    except ValueError:
        print("closed ValueError ok")
    try:
        zipfile.ZipFile(pg, "q")
    except ValueError:
        print("mode ValueError ok")
    try:
        with zipfile.ZipFile(pg, "w") as z:
            z.read("nonexistent")
    except KeyError:
        print("write-only KeyError ok")

    # bzip2/lzma 不支持
    try:
        with zipfile.ZipFile(d + "/bz.zip", "w", zipfile.ZIP_BZIP2) as z:
            z.writestr("a", "b")
    except NotImplementedError:
        print("bzip2 NI ok")
    try:
        with zipfile.ZipFile(d + "/lz.zip", "w", zipfile.ZIP_LZMA) as z:
            z.writestr("a", "b")
    except NotImplementedError:
        print("lzma NI ok")
    print("done")
finally:
    shutil.rmtree(d, ignore_errors=True)

# --- expect ---
# 8
# 1400
# b'plain'
# True
# encrypted RE ok
# FNF ok
# badzip ok
# False
# crc BadZip ok
# victim.txt
# trunc ok
# KeyError ok
# read KeyError ok
# closed ValueError ok
# mode ValueError ok
# write-only KeyError ok
# bzip2 NI ok
# lzma NI ok
# done
