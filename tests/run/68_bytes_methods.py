# bytes 方法：操作码必须按方法名选（之前一律走成 upper）
print(b"  x  ".strip())
print(b"  x  ".lstrip())
print(b"  x  ".rstrip())
print(b"xxaxx".strip(b"x"))
print(b"AB".lower())
print(b"ab".upper())
print(b"ab".replace(b"a", b"c"))
print(b"aaa".replace(b"a", b"b", 2))
print(b"abc".replace(b"b", b""))
print(b"a".decode())
print("a".encode())

# --- expect ---
# b'x'
# b'x  '
# b'  x'
# b'a'
# b'ab'
# b'AB'
# b'cb'
# b'bba'
# b'ac'
# a
# b'a'
