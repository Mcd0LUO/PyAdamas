# 标注决定存储类型：int 值出现在 float 位置时必须转成 double。
# 这条链路以前是漏的——`x: float = 1` 会把整数位模式当 double 读出来（5e-324）。

def scale(x: float, k: float) -> float:
    return x * k

def half(n: int) -> float:
    return n / 2

def main() -> None:
    a: float = 1
    print(a)

    b: float = 2
    b = 3
    print(b)

    print(scale(2, 1.5))
    print(half(5))

    xs: list[float] = [1, 2, 3]
    print(xs[0], xs[2])
    xs[1] = 7
    print(xs[1])

    print(f"{a} {b}")

main()

# --- expect ---
# 1.0
# 3.0
# 3.0
# 2.5
# 1.0 3.0
# 7.0
# 1.0 3.0
