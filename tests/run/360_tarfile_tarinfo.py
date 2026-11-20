# 360 tarfile 的 TarInfo 面：字段默认值、类型谓词、ustar 头
# tobuf/frombuf 往返（含 prefix 长名拆分、目录尾斜杠、符号链接、
# 非 ASCII 名）、校验和/截断/空头/溢出错误分支。
# 对拍 CPython 3.14：输出逐字节一致（头字节用 base64 钉死）。
# pyadamas: lang-gaps
import base64
import tarfile
from tarfile import TarInfo


t = TarInfo("a.txt")
print(t.name, t.mode, t.uid, t.gid, t.size, t.mtime, t.type, t.linkname)
print(t.uname, t.gname, t.devmajor, t.devminor, t.offset, t.offset_data, t.chksum)

print(t.isfile(), t.isreg(), t.isdir(), t.issym(), t.islnk())
print(t.isdev(), t.ischr(), t.isblk(), t.isfifo(), t.issparse())

types = [tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.LNKTYPE, tarfile.SYMTYPE,
         tarfile.CHRTYPE, tarfile.BLKTYPE, tarfile.DIRTYPE, tarfile.FIFOTYPE,
         tarfile.CONTTYPE]
for ty in types:
    ti = TarInfo("m")
    ti.type = ty
    print(ty, ti.isfile(), ti.isdir(), ti.issym(), ti.islnk(), ti.isdev(),
          ti.ischr(), ti.isblk(), ti.isfifo())

# ── ustar 头字节（与 CPython 逐字节一致）──────────────────────────
t.size = 12
t.mtime = 1700000000
t.mode = 0o640
h = t.tobuf(tarfile.USTAR_FORMAT, "utf-8", "surrogateescape")
print(len(h))
print(base64.b64encode(h).decode())

back = TarInfo.frombuf(h, "utf-8", "surrogateescape")
print(back.name, back.mode, back.size, back.mtime, back.type, back.chksum)
print(back.isfile(), back.isdir())

d = TarInfo("dir/sub")
d.type = tarfile.DIRTYPE
d.mode = 0o755
d.mtime = 1600000000
dh = d.tobuf(tarfile.USTAR_FORMAT, "utf-8", "surrogateescape")
print(base64.b64encode(dh).decode())
db = TarInfo.frombuf(dh, "utf-8", "surrogateescape")
print(db.name, db.type, db.isdir(), db.mode)

long_name = "prefix/" + "n" * 90
ln = TarInfo(long_name)
ln.mtime = 0
lh = ln.tobuf(tarfile.USTAR_FORMAT, "utf-8", "surrogateescape")
print(len(long_name.encode()), base64.b64encode(lh).decode())
lb = TarInfo.frombuf(lh, "utf-8", "surrogateescape")
print(lb.name == long_name, len(lb.name.encode()))

sy = TarInfo("ln")
sy.type = tarfile.SYMTYPE
sy.linkname = "hello.txt"
sy.mtime = 1700000000
sh = sy.tobuf(tarfile.USTAR_FORMAT, "utf-8", "surrogateescape")
print(base64.b64encode(sh).decode())
sb = TarInfo.frombuf(sh, "utf-8", "surrogateescape")
print(sb.name, sb.type, sb.issym(), sb.linkname)

u = TarInfo("é-unicode.txt")
u.mtime = 1700000000
uh = u.tobuf(tarfile.USTAR_FORMAT, "utf-8", "surrogateescape")
print(base64.b64encode(uh).decode())
ub = TarInfo.frombuf(uh, "utf-8", "surrogateescape")
print(ub.name, len(ub.name.encode()))

# ── 错误分支 ────────────────────────────────────────────────────
bad = h[:148] + b"000000\x00" + h[155:]
try:
    TarInfo.frombuf(bad, "utf-8", "surrogateescape")
except tarfile.InvalidHeaderError as e:
    print(type(e).__name__, str(e))
try:
    TarInfo.frombuf(b"short", "utf-8", "surrogateescape")
except tarfile.TruncatedHeaderError as e:
    print(type(e).__name__, str(e))
try:
    TarInfo.frombuf(b"\x00" * 512, "utf-8", "surrogateescape")
except tarfile.EOFHeaderError as e:
    print(type(e).__name__, str(e))
try:
    TarInfo.frombuf(b"", "utf-8", "surrogateescape")
except tarfile.EmptyHeaderError as e:
    print(type(e).__name__, str(e))
try:
    big = TarInfo("big")
    big.size = 8 ** 11
    big.tobuf(tarfile.USTAR_FORMAT, "utf-8", "surrogateescape")
except ValueError as e:
    print("overflow", str(e))
try:
    TarInfo("l" * 300).tobuf(tarfile.USTAR_FORMAT, "utf-8", "surrogateescape")
except ValueError as e:
    print("toolong", str(e))

print(tarfile.BLOCKSIZE, tarfile.RECORDSIZE, tarfile.LENGTH_NAME, tarfile.LENGTH_PREFIX)
print(tarfile.USTAR_FORMAT, tarfile.GNU_FORMAT, tarfile.PAX_FORMAT, tarfile.DEFAULT_FORMAT)
print(tarfile.ENCODING)
print(tarfile.BLOCKSIZE == 512, tarfile.LENGTH_LINK == 100)
print(TarInfo("r").__repr__().startswith("<TarInfo"))
# --- expect ---
# a.txt 420 0 0 0 0 b'0' 
#   0 0 0 0 0
# True True False False False
# False False False False False
# b'0' True False False False False False False False
# b'\x00' True False False False False False False False
# b'1' False False False True False False False False
# b'2' False False True False False False False False
# b'3' False False False False True True False False
# b'4' False False False False True False True False
# b'5' False True False False False False False False
# b'6' False False False False True False False True
# b'7' True False False False False False False False
# 512
# YS50eHQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAwMDA2NDAAMDAwMDAwMAAwMDAwMDAwADAwMDAwMDAwMDE0ADE0NTI0NzcwNDAwADAwNjc1NwAgMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB1c3RhcgAwMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
# a.txt 416 12 1700000000 b'0' 3567
# True False
# ZGlyL3N1Yi8AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAwMDA3NTUAMDAwMDAwMAAwMDAwMDAwADAwMDAwMDAwMDAwADEzNzI3NDEwMDAwADAwNzM0NQAgNQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB1c3RhcgAwMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
# dir/sub b'5' True 493
# 97 cHJlZml4L25ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubm5ubgAAADAwMDA2NDQAMDAwMDAwMAAwMDAwMDAwADAwMDAwMDAwMDAwADAwMDAwMDAwMDAwADAzMjUwNgAgMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB1c3RhcgAwMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
# True 97
# bG4AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAwMDA2NDQAMDAwMDAwMAAwMDAwMDAwADAwMDAwMDAwMDAwADE0NTI0NzcwNDAwADAxMDE3NQAgMmhlbGxvLnR4dAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB1c3RhcgAwMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
# ln b'2' True hello.txt
# w6ktdW5pY29kZS50eHQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADAwMDA2NDQAMDAwMDAwMAAwMDAwMDAwADAwMDAwMDAwMDAwADE0NTI0NzcwNDAwADAxMTAxNQAgMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAB1c3RhcgAwMAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=
# é-unicode.txt 14
# InvalidHeaderError bad checksum
# TruncatedHeaderError truncated header
# EOFHeaderError end of file header
# EmptyHeaderError empty header
# overflow overflow in number field
# toolong name is too long
# 512 10240 100 155
# 0 1 2 2
# utf-8
# True True
# True
