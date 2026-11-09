# with：__enter__/__exit__、as 绑定、return/break/continue 与嵌套
class Ctx:
    def __init__(self, tag: int) -> None:
        self.tag = tag
        self.entered = 0
        self.exited = 0

    def __enter__(self):
        self.entered = self.entered + 1
        return self

    def __exit__(self, a, b, c):
        self.exited = self.exited + 1
        return False

def normal() -> int:
    with Ctx(1) as c:
        print(c.tag)
        print(c.entered)
    return c.exited

def early(r: int) -> int:
    with Ctx(2) as c:
        if r > 0:
            return 100 + c.exited
    return 0

def loop() -> int:
    total: int = 0
    total = total + normal()
    total = total + early(0)
    total = total + early(1)
    for i in range(4):
        with Ctx(i) as c:
            if i == 1:
                continue
            if i == 2:
                break
            total = total + c.entered
    return total

def nested() -> None:
    with Ctx(7) as a:
        with Ctx(8) as b:
            print(a.tag + b.tag)
        print(a.entered + b.exited)

print(normal())
print(loop())
nested()
# --- expect ---
# 1
# 1
# 1
# 1
# 1
# 102
# 15
# 2
