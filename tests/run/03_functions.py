# 递归、默认参数、多参数、栈传参

def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def default_args(a: int, b: int = 5, c: int = 9) -> int:
    return a * 100 + b * 10 + c

def many(a: int, b: int, c: int, d: int, e: int, f: int, g: int, h: int) -> int:
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8

def fargs(a: float, b: float, c: float, d: float, e: float, f: float,
          g: float, h: float, i: float, j: float) -> float:
    return a + b + c + d + e + f + g + h + i + j

def mixed(a: int, x: float, b: str, y: float) -> str:
    return b + str(a) + "," + str(x) + "," + str(y)

print(fib(10))
print(default_args(1, c=9))
print(many(1, 2, 3, 4, 5, 6, 7, 8))
print(fargs(1.1, 2.2, 3.3, 4.4, 5.5, 6.6, 7.7, 8.8, 9.9, 10.1))
print(mixed(7, 1.5, "ok", 2.5))

# --- expect ---
# 55
# 159
# 204
# 59.6
# ok7,1.5,2.5
