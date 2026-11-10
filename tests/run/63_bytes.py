x = b"abc"
print(x.decode())
print("hi".encode())
print(b"xyz".upper())
print(x.decode().upper())
print(len(b"abcd"))
print(b"a" + b"b" == b"ab")
print(b"abc"[0])
print(b"abc"[1:])
print(b"a" in b"abc")
print(b"q'uote")
print(b"\x00\xff")
print(b"a\nb")

# --- expect ---
# abc
# b'hi'
# b'XYZ'
# ABC
# 4
# True
# 97
# b'bc'
# True
# b"q'uote"
# b'\x00\xff'
# b'a\nb'
