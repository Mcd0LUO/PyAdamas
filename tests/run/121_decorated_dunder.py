# 装饰 dunder 方法：调用点从类属性取包装函数并补 self。算术/比较/str/
# len/hash/iter/contains 都走 dunder_call；构造、with/async with 和
# __call__ 见 126_decorated_slots.py。
def wrap2(f):
    def wrapper(self, o) -> bool:
        return f(self, o)
    return wrapper


def wrap_len(f):
    def wrapper(self) -> int:
        return f(self) + 10
    return wrapper


def wrap_iter(f):
    def wrapper(self) -> Generator[int]:
        return f(self)
    return wrapper


def wrap_hash(f):
    def wrapper(self) -> int:
        return f(self)
    return wrapper


def wrap_in(f):
    def wrapper(self, x) -> bool:
        return f(self, x)
    return wrapper


def wrap_add(f):
    def wrapper(self, o: int) -> int:
        return f(self, o) + 1000
    return wrapper


def wrap_str(f):
    def wrapper(self) -> str:
        return "<" + f(self) + ">"
    return wrapper


class W:
    def __init__(self, v: int) -> None:
        self.v = v

    @wrap2
    def __eq__(self, o) -> bool:
        return self.v == o

    @wrap_len
    def __len__(self) -> int:
        return self.v

    @wrap_iter
    def __iter__(self):
        yield self.v
        yield self.v + 1

    @wrap_hash
    def __hash__(self) -> int:
        return self.v * 7

    @wrap_in
    def __contains__(self, x) -> bool:
        return x > self.v

    @wrap_add
    def __iadd__(self, o: int) -> int:
        return self.v + o

    @wrap_str
    def __str__(self) -> str:
        return str(self.v)


w = W(3)
print(w == 3, w == 4)
print(len(w))
out = []
for x in w:
    out.append(x)
print(out)
print(hash(w) == 21)
print(1 in w, 9 in w)
print(str(w))
print(w)
w += 2
print(w)

# --- expect ---
# True False
# 13
# [3, 4]
# True
# False True
# <3>
# <3>
# 1005
