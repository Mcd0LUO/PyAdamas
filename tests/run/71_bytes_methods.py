b = b"hello world"
print(b.split())
print(b.split(b"o"))
print(b.find(b"o"))
print(b.rfind(b"o"))
print(b.count(b"o"))
print(b.startswith(b"he"))
print(b.endswith(b"ld"))
print(b",".join([b"a", b"b"]))
print(b.find(b"zz"))
print(b"".join([]))
# --- expect ---
# [b'hello', b'world']
# [b'hell', b' w', b'rld']
# 4
# 7
# 2
# True
# True
# b'a,b'
# -1
# b''
