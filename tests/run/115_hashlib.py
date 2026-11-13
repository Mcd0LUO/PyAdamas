# hashlib 子集：MD5 / SHA-1 / SHA-256 的标准向量与增量更新。
import hashlib


def mkdata(n: int) -> bytes:
    xs: list[int] = []
    i = 0
    while i < n:
        xs.append((i * 37 + 11) % 256)
        i += 1
    return bytes(xs)


print(hashlib.md5(b"").hexdigest())
print(hashlib.md5(b"abc").hexdigest())
print(len(hashlib.md5(b"abc").digest()))

c = hashlib.md5()
c.update(b"The quick brown fox ")
c.update(b"jumps over the lazy dog")
print(c.hexdigest())

print(hashlib.sha1(b"").hexdigest())
print(hashlib.sha1(b"abc").hexdigest())
print(hashlib.sha256(b"").hexdigest())
print(hashlib.sha256(b"abc").hexdigest())
print(hashlib.sha256(b"The quick brown fox jumps over the lazy dog").hexdigest())

# 200 字节、跨多个 64 字节块
big = mkdata(200)
print(hashlib.md5(big).hexdigest())
print(hashlib.sha1(big).hexdigest())
print(hashlib.sha256(big).hexdigest())

# --- expect ---
# d41d8cd98f00b204e9800998ecf8427e
# 900150983cd24fb0d6963f7d28e17f72
# 16
# 9e107d9d372bb6826bd81d3542a419d6
# da39a3ee5e6b4b0d3255bfef95601890afd80709
# a9993e364706816aba3e25717850c26c9cd0d89d
# e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
# ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
# d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592
# f9914cd8b67085ceb02d1eba3ff2dc0c
# 588cf108f067a9951c1d42248e0c3a6e49afe002
# 5e48d64839a8bff5e2471507af6a49f9990817df437857f4a2007df41266e1b5
