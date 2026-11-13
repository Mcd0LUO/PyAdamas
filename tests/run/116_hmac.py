# hmac 子集：HMAC-MD5 / SHA-1 / SHA-256，与 CPython 结果一致。
import hmac

m = hmac.new(b"key", b"The quick brown fox jumps over the lazy dog", "sha256")
print(m.hexdigest())

print(hmac.new(b"key", digestmod="sha1").hexdigest())
print(hmac.new(b"key", b"", "md5").hexdigest())

# 超过 64 字节的 key 先做一次 hash
ks: list[int] = []
i = 0
while i < 100:
    ks.append(i)
    i += 1
print(hmac.new(bytes(ks), b"msg", "sha256").hexdigest())

# 增量 update
h = hmac.new(b"key", digestmod="sha256")
h.update(b"The quick brown fox ")
h.update(b"jumps over the lazy dog")
print(h.hexdigest())

# --- expect ---
# f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
# f42bb0eeb018ebbd4597ae7213711ec60760843f
# 63530468a04e386459855da0063b6596
# f85da02f25a44a117825adec49678dd31f98d263ba21680c07fd30c161cda4ec
# f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
