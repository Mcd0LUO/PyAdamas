# 非负被除数的常数除数除法/取模（无符号魔数路径）回归。
#
# 降级期给可证非负的 vreg 打标（非负常量 / len / 非负槽 load /
# and/shr/mod 传播）；start 可证非负且 step 1 的 range 循环标记计数器
# 与循环变量。被除数非负、除数为正的常数除法/取模走无符号魔数路径
#（省 |x| 与符号还原，2 的幂 mod 退化成一条 AND）。钉住等价性：
#   1. 正常循环 i%3 / i%7 / i//3 / i//7 / i%8 / i//8；
#   2. 负 start 仍走有符号路径；体内写负值作废提示；
#   3. step 2 / while / 循环外使用不标记；
#   4. INT64_MAX 边界（无符号魔数不得溢出）；
#   5. 负除数 / 变量除数走原路径；
#   6. 嵌套同名变量、组合提示（内层 start 来自外层循环变量）；
#   7. and/shr/mod 传播；生成器里可用。

IMAX = 9223372036854775807

s = 0
for i in range(200):
    s += i % 3 + i % 7 + i // 3 + i // 7 + i % 8 + i // 8
print("loop", s)

s = 0
for i in range(-10, 10):
    s += i % 3 + i // 3 + i % 8 + i // 8
print("negstart", s)

s = 0
for i in range(10):
    i = -5
    s += i % 3 + i // 3
print("reassign", s)

s = 0
for i in range(0, 20, 2):
    s += i % 3 + i // 3
print("step2", s)

s = 0
i = 0
while i < 10:
    s += i % 3
    i += 1
print("while", s)

for i in range(5):
    pass
print("after", i % 3, i // 3)

# 逐项打印：累加和会超 int64（pyadamas 加法回绕是既有语义，
# 不是本用例要钉的东西），边界值本身每项都在 int64 内。
out = []
for i in range(IMAX - 4, IMAX):
    out.append((i % 3, i % 7, i // 3, i % 8, i // 8))
print("imax", out)

s = 0
for i in range(10):
    s += i % -3 + i // -3
print("negdiv", s)

d = 3
s = 0
for i in range(10):
    s += i % d + i // d
print("vardiv", s)

s = 0
for i in range(3):
    for i in range(4):
        s += i % 3
    s += i % 3
print("nested", s)

s = 0
for a in range(3):
    for b in range(a, a + 4):
        s += b % 3 + b // 3
print("compose", s)

s = 0
for i in range(20):
    s += (i & 7) % 3 + (i >> 1) % 3 + (i % 5) % 3
print("prop", s)

xs = [1, 2, 3, 4, 5]
s = 0
for k in range(len(xs)):
    s += k % 3 + xs[k]
print("len", s)

def gen():
    t = 0
    for i in range(30):
        t += i % 7
    yield t
print("gen", next(gen()))
# --- expect ---
# loop 13218
# negstart 70
# reassign -10
# step2 36
# while 9
# after 1 1
# imax [(0, 3, 3074457345618258601, 3, 1152921504606846975), (1, 4, 3074457345618258601, 4, 1152921504606846975), (2, 5, 3074457345618258601, 5, 1152921504606846975), (0, 6, 3074457345618258602, 6, 1152921504606846975)]
# negdiv -27
# vardiv 21
# nested 9
# compose 18
# prop 51
# len 19
# gen 85
