xs = [1, 2, 3]
print(xs[0], xs[-1], xs[-3])
xs[-1] = 9
print(xs[2])
xs[0] = 7
print(xs[0], xs[1], xs[2])

ys: list[str] = ["a", "b"]
ys[0] = "c"
print(ys[0], ys[-1])

zs: list[float] = [1.5, 2.5]
zs[1] = 7.5
print(zs[1], zs[-2])

try:
    print(xs[3])
except IndexError as e:
    print("oob", str(e))

try:
    xs[-4] = 1
except IndexError as e:
    print("soob", str(e))

ts = (10, 20, 30)
print(ts[-1], ts[1])

# --- expect ---
# 1 3 1
# 9
# 7 2 9
# c b
# 7.5 1.5
# oob list index out of range
# soob list assignment index out of range
# 30 20
