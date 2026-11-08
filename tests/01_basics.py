# 基础：字面量、算术、赋值、字符串
x = 1
y = 2.5
name = "py23"
flag = True
nothing = None

z = x + y * 2 - 3 / 1.5
z //= 2
z **= 2
z %= 7

s = "a" + "b" * 3
t = 'it\'s ok'
u = """triple
quoted"""

a, b = 1, 2
a, b = b, a

n: int = 42
ratio: float = 1.0

# 位运算与比较
m = (1 << 4) | (3 & 2) ^ (~0)
c = 1 < 2 <= 3 != 4
d = x is not None
e = "a" in "abc"
f = 3 not in [1, 2]

print(x, y, name, flag, nothing, sep=", ", end="\n")
