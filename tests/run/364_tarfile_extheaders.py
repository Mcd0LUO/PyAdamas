# 364 tarfile 的扩展头与 format 参数：GNU_FORMAT/PAX_FORMAT 写入往返、
# 手搓 GNU 长名（L）/长链接（K）与 pax 扩展头（x，含 size 覆盖）的读取、
# ustar prefix 长名（183 字节）整档往返。
# 对拍 CPython 3.14：输出逐字节一致。
# pyadamas: lang-gaps
import os.path as _osp
import tempfile
import tarfile
from io import BytesIO
from tarfile import TarInfo
from typing import cast


d = tempfile.mkdtemp()


def pad512(b: bytes) -> bytes:
    rem = len(b) % 512
    if rem > 0:
        return b + b"\x00" * (512 - rem)
    return b


def gnu_payload(s: str) -> bytes:
    b = s.encode("utf-8")
    return b + b"\x00" * (512 - len(b))


def pax_record(k: str, v: str) -> str:
    rec = " " + k + "=" + v + "\n"
    n = len(rec) + 1
    k2 = len(rec) + len(str(n))
    while k2 != n:
        n = k2
        k2 = len(rec) + len(str(n))
    return str(n) + rec


def gnu_long_blocks(longname: str, longlink: str, mt: int) -> bytes:
    out = b""
    if len(longlink) > 0:
        lk = TarInfo("link")
        lk.type = tarfile.GNUTYPE_LONGLINK
        lk.size = len(longlink.encode("utf-8"))
        lk.mtime = mt
        out = out + lk.tobuf() + gnu_payload(longlink)
    if len(longname) > 0:
        ln = TarInfo("name")
        ln.type = tarfile.GNUTYPE_LONGNAME
        ln.size = len(longname.encode("utf-8"))
        ln.mtime = mt
        out = out + ln.tobuf() + gnu_payload(longname)
    return out


def pax_ext_block(path: str, size: int, mt: int) -> bytes:
    body = pax_record("path", path) + pax_record("size", str(size)) + pax_record("mtime", str(mt))
    h = TarInfo("paxheader")
    h.type = tarfile.XHDTYPE
    h.size = len(body.encode("utf-8"))
    h.mtime = mt
    return h.tobuf() + pad512(body.encode("utf-8"))


# ── 1) format=GNU_FORMAT / PAX_FORMAT 写入往返 ───────────────────
for fmt, tag in ((tarfile.GNU_FORMAT, "gnu"), (tarfile.PAX_FORMAT, "pax")):
    p = d + "/w-" + tag + ".tar"
    tf = tarfile.open(p, "w", format=fmt)
    a = TarInfo("one.txt")
    a.size = 3
    a.mtime = 1700000000
    tf.addfile(a, BytesIO(b"one"))
    b = TarInfo("two.txt")
    b.size = 3
    b.mtime = 1700000001
    tf.addfile(b, BytesIO(b"two"))
    tf.close()
    with tarfile.open(p) as rd:
        print(tag, rd.getnames())
        print(tag, rd.extractfile("one.txt").read(), rd.extractfile("two.txt").read())
        print(tag, _osp.getsize(p) % 10240 == 0)

# ── 2) 手搓 GNU 长名/长链接 ─────────────────────────────────────
longname = "g/" + "n" * 140 + ".txt"
longlink = "t/" + "k" * 130 + ".txt"
mt = 1700000000
real = TarInfo("truncated-name")
real.size = 4
real.mtime = mt
raw = gnu_long_blocks(longname, longlink, mt) + real.tobuf() + b"data" + b"\x00" * 508
raw = raw + b"\x00" * 1024
p2 = d + "/gnu.tar"
f = open(p2, "wb")
f.write(cast(str, raw))
f.close()
with tarfile.open(p2) as rd2:
    print(rd2.getnames())
    print(rd2.getnames()[0] == longname, len(rd2.getnames()[0]))
    m = rd2.getmember(longname)
    print(m.size, m.mtime, m.type)
    print(rd2.extractfile(longname).read())

lk_blocks = gnu_long_blocks("lnk", longlink, mt)
sl = TarInfo("lnk")
sl.type = tarfile.SYMTYPE
sl.linkname = "short.txt"
sl.mtime = mt
raw2 = lk_blocks + sl.tobuf() + b"\x00" * 1024
p3 = d + "/gnu2.tar"
f = open(p3, "wb")
f.write(cast(str, raw2))
f.close()
with tarfile.open(p3) as rd3:
    m2 = rd3.getmember("lnk")
    print(m2.type, m2.issym(), m2.linkname == longlink, len(m2.linkname))

# ── 3) 手搓 pax 扩展头（path + size 覆盖 + mtime）─────────────────
paxpath = "pax/" + "p" * 120 + ".bin"
body_size = 5
hdr = TarInfo("trunc")
hdr.size = body_size
hdr.mtime = 1
raw3 = pax_ext_block(paxpath, body_size, 1700000123) + hdr.tobuf() + b"pax!!" + b"\x00" * 507
raw3 = raw3 + b"\x00" * 1024
p4 = d + "/pax.tar"
f = open(p4, "wb")
f.write(cast(str, raw3))
f.close()
with tarfile.open(p4) as rd4:
    print(rd4.getnames()[0] == paxpath, len(rd4.getnames()[0]))
    m3 = rd4.getmember(paxpath)
    print(m3.size, m3.mtime == 1700000123, m3.type)
    print(rd4.extractfile(paxpath).read())

# ── 4) ustar prefix 长名整档（183 字节名，两实现都拆 prefix）──────
ustar_long = "d" * 60 + "/" + "n" * 60 + "/" + "f" * 60 + ".txt"
p5 = d + "/long.tar"
tf5 = tarfile.open(p5, "w", format=tarfile.USTAR_FORMAT)
t5 = TarInfo(ustar_long)
t5.size = 4
t5.mtime = mt
tf5.addfile(t5, BytesIO(b"long"))
tf5.close()
print(_osp.getsize(p5))
with tarfile.open(p5) as rd5:
    print(rd5.getnames()[0] == ustar_long, len(rd5.getnames()[0]))
    print(rd5.extractfile(ustar_long).read())
# --- expect ---
# gnu ['one.txt', 'two.txt']
# gnu b'one' b'two'
# gnu True
# pax ['one.txt', 'two.txt']
# pax b'one' b'two'
# pax True
# ['g/nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn.txt']
# True 146
# 4 1700000000 b'0'
# b'data'
# b'2' True True 136
# True 128
# 5 True b'0'
# b'pax!!'
# 10240
# True 186
# b'long'
