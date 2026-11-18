# 同一虚调用点的两条释放路径：旧对象独占时执行内嵌生成器的 finally，
# 改写接收者和累加器；由 replacements 持有旧对象时可走无重入快路。
class Meter:
    def __init__(self, n: int, g: Generator[int, None, None]) -> None:
        self.n = n
        self.g = g
    def get(self) -> int:
        return self.n

class Square(Meter):
    def get(self) -> int:
        return self.n * self.n

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

replacements: list[Meter] = [Meter(2, stream(2)), Meter(4, stream(4)), Meter(5, stream(5))]
print(next(current.g))
for i in range(3):
    current = replacements[i]
    total += current.get()
print(total)

# --- expect ---
# 1
# 28
