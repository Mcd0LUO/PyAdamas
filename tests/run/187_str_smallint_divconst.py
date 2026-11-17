# 小整数 -> 字符串，以及常数除数的除法/取模
#
# 这一组钉住两条优化路径的边界：
#
#   1. pa_str_from_i64 对 0..9 返回驻留单字节串（零分配）。驻留表按
#      **字符编码**索引，所以 str(7) 必须拿 '7'（0x37）而不是 7（BEL）。
#      这个 bug 差分测试没抓到 —— 原来没有任何用例在 str(单个数字) 之后
#      再打印或比较。这里补上。
#   2. 常数除数的除法走「魔数乘法 + 符号修正」而不是 idivq。负数、INT64
#      两端、有符号取整方向都要和 CPython 逐位一致。

digits = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
for d in digits:
    s = str(d)
    print(s, len(s), s == str(d), ord(s), int(s) + 1)

print("concat", "a" + str(7) + "b", "x" + str(0) + "y")
print("join", "".join([str(1), str(2), str(3)]))
print("sep", str(1), str(2), str(3))

line = "0123456789"
out = ""
i = 0
while i < len(line):
    out = out + "[" + line[i] + "]"
    i += 1
print("walk", out)

# 单字符比较（走 pa_str_eq 的 n == 1 快速路径）
hits = 0
i = 0
while i < len(line):
    if line[i] == "7":
        hits += 1
    if line[i] != "3":
        hits += 10
    i += 1
print("cmp1", hits)

# 驻留对象是共享的，不能被谁改坏
a = str(4)
b = str(4)
print("same", a == b, a + b, len(a + b))

# ── 常数除数 ──────────────────────────────────────────────
# 逐条打印太长，压成一个校验和：任一 (商, 余) 错位都会让 sum 变。
vals = [0, 1, -1, 7, -7, 10, -10, 100, -100, 12345, -12345, 2147483647,
        -2147483647, 2147483648, -9223372036854775806, 9223372036854775807]
divs = [2, 3, 7, 10, 100, 1024, 1000, -2, -3, -7, -10, -1000]
checksum = 0
for v in vals:
    for d in divs:
        q = v // d
        r = v % d
        checksum = checksum + q * 31 + r * 7
print("divsum", checksum)

# 显式钉住几条边界：负数取整方向、INT64 两端
print("neg", -7 // 3, -7 % 3, 7 // -3, 7 % -3, -7 // -3, -7 % -3)
imin = -9223372036854775807 - 1
print("min", imin // 2, imin % 3)
print("max", 9223372036854775807 // 3, 9223372036854775807 % 3)
print("one", 5 // 1, 5 % 1, 5 // -1, 5 % -1)

# 运行时除零必须报错（编译期常量 0 不参与化简，这里走的是变量）
x = 5
y = 0
if y != 0:
    print("never", x // y)

# 循环里对常数取模 —— 最容易退化成 idivq 的形态
total = 0
i = 0
while i < 1000:
    if i % 3 == 0:
        total += 1
    if i % 7 == 0:
        total += 10
    total += i % 10
    i += 1
print("loopmod", total)

# --- expect ---
# 0 1 True 48 1
# 1 1 True 49 2
# 2 1 True 50 3
# 3 1 True 51 4
# 4 1 True 52 5
# 5 1 True 53 6
# 6 1 True 54 7
# 7 1 True 55 8
# 8 1 True 56 9
# 9 1 True 57 10
# concat a7b x0y
# join 123
# sep 1 2 3
# walk [0][1][2][3][4][5][6][7][8][9]
# cmp1 91
# same True 44 2
# divsum 730786208
# neg -3 2 -3 -2 2 -1
# min -4611686018427387904 1
# max 3074457345618258602 1
# one 5 0 -5 0
# loopmod 6264
