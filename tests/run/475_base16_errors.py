# base64 子集语义钉桩：b16 错误口径（CPython 实跑取期望）。
# base64 子集语义钉桩：b16/b32 错误路径（CPython 实跑取期望）。
import base64
import binascii

for c in [b"zz", b"WW", b"0g", b"a"]:
    try:
        print(len(base64.b16decode(c)))
    except ValueError as e:
        print("ValueError", str(e))
print(base64.b16decode(b"48656C6C6F"))
print(binascii.hexlify(base64.b16decode(b"4A")).decode())
print(len(base64.b32encode(b"hi")), base64.b32decode(b"NBUQ===="))
# --- expect ---
# ValueError Non-base16 digit found
# ValueError Non-base16 digit found
# ValueError Non-base16 digit found
# ValueError Non-base16 digit found
# b'Hello'
# 4a
# 8 b'hi'
