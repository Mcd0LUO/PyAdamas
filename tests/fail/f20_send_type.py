# expect: send() expects int

def acc() -> Generator[int, int, None]:
    while True:
        x = yield 0


g = acc()
g.send("oops")
