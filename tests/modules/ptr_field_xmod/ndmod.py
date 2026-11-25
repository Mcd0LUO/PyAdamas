cimport clib as c


class Arr:
    data: ptr[float]
    n: int

    def __init__(self, n: int) -> None:
        self.n = n
        self.data = c.malloc_f(n * 8)

    def get(self, i: int) -> float:
        return self.data[i]

    def close(self) -> None:
        c.free(self.data)
