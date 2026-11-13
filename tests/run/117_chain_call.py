# 链式调用：跨模块函数返回对象后直接调方法，以及构造后直接调方法。
import hashlib

print(hashlib.md5(b"abc").hexdigest())
print(hashlib.sha256(b"abc").hexdigest())
print(len(hashlib.sha1(b"abc").digest()))


class Box:
    def __init__(self, v: int) -> None:
        self.v = v

    def add(self, x: int) -> int:
        return self.v + x


print(Box(3).add(4))
print(Box(5).add(Box(1).add(2)))

# --- expect ---
# 900150983cd24fb0d6963f7d28e17f72
# ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
# 20
# 7
# 8
