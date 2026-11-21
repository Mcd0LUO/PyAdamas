from bigops import erase, low4, sq, put, has

a: big = 18446744073709551623
x: object = erase(a)
y: object = erase(a + 1 - 1)
print(low4(x), sq(x))
d: dict[object, int] = {}
put(d, x, 7)
print(has(d, y), len(d))
# --- expect ---
# 7 340282366920938463721629024463701934129
# True 1
