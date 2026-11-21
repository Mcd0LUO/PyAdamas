def take(x: big) -> big:
    return x

def ret() -> big:
    return -456

a: big = -123
b: big = +123
c: big = -(-9)
print(a, b, c, a >> 3, b >> 3)
print(take(-7), ret(), +a, -a)
count: object = 5
print(b << cast_big(count))

# --- expect ---
# -123 123 9 -16 15
# -7 -456 -123 123
# 3936
