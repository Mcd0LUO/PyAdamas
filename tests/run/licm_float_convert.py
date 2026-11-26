def f(n: int, x: float) -> int:
    i = 0
    y = 0
    while i < n:
        y = int(x)
        i = i + 1
    a = 0
    while a < 3:
        a = a + 1
    b = 0
    while b < 3:
        b = b + 1
    c = 0
    while c < 3:
        c = c + 1
    d = 0
    while d < 3:
        d = d + 1
    e = 0
    while e < 3:
        e = e + 1
    return y + a + b + c + d + e

def main() -> int:
    r = 0
    try:
        r = f(0, float("inf"))
    except OverflowError:
        r = 99
    print(r)
    return 0

main()

# --- expect ---
# 15
