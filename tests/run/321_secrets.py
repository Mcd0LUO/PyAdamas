# secrets 子集：令牌生成/区间随机/恒定时间比较（随机源是 pa random）
import secrets

tb = secrets.token_bytes(8)
print(len(tb), isinstance(tb, bytes))
print(len(secrets.token_bytes()), len(secrets.token_bytes(0)))

th = secrets.token_hex(4)
print(len(th), len(secrets.token_hex()))
ok = True
for ch in th:
    if ch not in "0123456789abcdef":
        ok = False
print(ok)

tu = secrets.token_urlsafe(9)
print(len(tu), "=" in tu, len(secrets.token_urlsafe(0)))
print(len(secrets.token_urlsafe()))

r = secrets.randbelow(10)
print(r >= 0 and r < 10)
print(secrets.randbelow(1))
print(secrets.randbits(0))
print(secrets.randbits(8) >= 0)
print(secrets.randbits(8) < 256)
print(secrets.randbits(32) >= 0)

for _ in range(5):
    v = secrets.randbelow(3)
    if v < 0 or v >= 3:
        print("bad randbelow")

x = secrets.choice([1, 2, 3])
print(x in [1, 2, 3])
c = secrets.choice("abc")
print(c in "abc")
try:
    secrets.choice([])
except IndexError:
    print("choice IndexError")
try:
    secrets.randbelow(0)
except ValueError:
    print("randbelow ValueError")
try:
    secrets.randbits(-1)
except ValueError:
    print("randbits ValueError")

print(secrets.compare_digest("abc", "abc"))
print(secrets.compare_digest("abc", "abd"))
print(secrets.compare_digest("abc", "ab"))
print(secrets.compare_digest(b"xy", b"xy"))
print(secrets.compare_digest(b"xy", b"xz"))


# --- expect ---
# 8 True
# 32 0
# 8 64
# True
# 12 False 0
# 43
# True
# 0
# 0
# True
# True
# True
# True
# True
# choice IndexError
# randbelow ValueError
# randbits ValueError
# True
# False
# False
# True
# False
