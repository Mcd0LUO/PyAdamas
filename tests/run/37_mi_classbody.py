class A:
    def hello(self) -> str:
        return "A"

    def name(self) -> str:
        return "A"


class B:
    def bye(self) -> str:
        return "B"

    def name(self) -> str:
        return "B"


class C(A, B):
    pass


class D(B, A):
    pass


c = C()
print(c.hello(), c.bye())
d = D()
print(d.hello(), d.bye())
print(C().name(), D().name())


def log(s: str) -> None:
    print("body", s)


class E(A, B):
    print("defining")
    if 1 < 2:
        flag = 1
    log("after")

    def info(self) -> int:
        return flag


e = E()
print(e.hello(), e.bye(), e.info())

# --- expect ---
# A B
# A B
# A B
# defining
# body after
# A B 1
