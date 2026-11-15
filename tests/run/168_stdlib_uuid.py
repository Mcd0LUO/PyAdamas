"""stdlib/uuid.py：uuid4 的格式与 UUID 字符串解析。"""
import uuid

u = uuid.uuid4()
s = str(u)
print(len(s))
print(s[8], s[13], s[18], s[23])
print(len(u.hex))
print(u.hex == s.replace("-", ""))
fixed = uuid.UUID("6ba7b810-9dad-11d1-80b4-00c04fd430c8")
print(str(fixed))
print(str(uuid.UUID("6ba7b8109dad11d180b400c04fd430c8")) == str(fixed))
print(len(repr(u)) > 30)

# --- expect ---
# 36
# - - - -
# 32
# True
# 6ba7b810-9dad-11d1-80b4-00c04fd430c8
# True
# True
