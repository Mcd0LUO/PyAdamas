# 守卫内联结果直接加到调用前的全局快照；失配时方法可读写全局或抛出。
# 抛异常后不能提交本轮的外层加法。
class Meter:
    def __init__(self, n: int, g: Generator[int, None, None]) -> None:
        self.n = n
        self.g = g
    def get(self) -> int:
        return self.n

class Square(Meter):
    def get(self) -> int:
        return self.n * self.n

class Observing(Meter):
    def get(self) -> int:
        global total
        print("saw", total)
        total = 1000
        return self.n

class Throwing(Meter):
    def get(self) -> int:
        global total
        print("throw saw", total)
        total += 100
        raise ValueError("slow call")


def stream(n: int):
    try:
        yield n
    finally:
        if n == 1:
            install()

current: Meter = Meter(1, stream(1))
total = 5

def install() -> None:
    global current, total
    current = Square(3, stream(3))
    total = 10

replacements: list[Meter] = [Meter(2, stream(2)), Observing(4, stream(4)), Meter(5, stream(5)), Throwing(7, stream(7))]
print(next(current.g))
try:
    for i in range(4):
        current = replacements[i]
        total += current.get()
except ValueError:
    print("caught", total)
print("final", total)

# --- expect ---
# 1
# saw 19
# throw saw 28
# caught 128
# final 128
