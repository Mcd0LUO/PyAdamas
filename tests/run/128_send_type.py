# 128：Generator[Y, S, R] 标注收窄 send 的静态类型

def acc(total: int) -> Generator[int, int, None]:
    while total < 100:
        x = yield total
        total = total + x


def tagger() -> Generator[str, str, None]:
    x = yield "start"
    while True:
        x = yield "got " + x


g = acc(0)
print(next(g))
print(g.send(5))
print(g.send(10))
g.close()

t = tagger()
print(next(t))
print(t.send("a"))
print(t.send("bc"))
t.close()

# 只标注 yield 类型时 send 不限（未知）
def plain() -> Generator[int]:
    x = yield 1
    while x != 0:
        x = yield x


p = plain()
print(next(p))
print(p.send(4))
p.close()

# --- expect ---
# 0
# 5
# 15
# start
# got a
# got bc
# 1
# 4
