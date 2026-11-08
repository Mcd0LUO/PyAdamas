# expect: cannot assign to method 'f'
class Bad:
    def f(self) -> None:
        pass

b: Bad = Bad()
b.f = 1
