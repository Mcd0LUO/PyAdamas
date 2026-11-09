class A:
    x: int

    def __init__(self, x: int) -> None:
        self.x = x

    def who(self) -> str:
        return "A"


class B(A):
    def __init__(self, x: int) -> None:
        super().__init__(x + 1)

    def who(self) -> str:
        return "B" + super().who()


class C(A):
    def __init__(self, x: int) -> None:
        super().__init__(x * 2)

    def who(self) -> str:
        return "C" + super().who()


class D(B, C):
    def __init__(self, x: int) -> None:
        super().__init__(x)

    def who(self) -> str:
        return "D" + super().who()


class E(C, B):
    def who(self) -> str:
        return "E" + super().who()


d = D(1)
print(d.who(), d.x)
e = E(1)
print(e.who(), e.x)
b = B(2)
print(b.who(), b.x)

# --- expect ---
# DBCA 4
# ECBA 3
# BA 3
