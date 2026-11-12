# expect: expects list[float] for 'ptr[float]'
cimport clib

xs: list[int] = [0]
print(clib.modf(1.5, xs))
