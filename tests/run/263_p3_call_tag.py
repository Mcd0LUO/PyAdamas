# 263_p3_call_tag —— 0.328 P3-1b：tag 过直调形参/返回（双字 ABI 旁路）
#
# 门控模块内直调本地函数：unknown 形参的 tag 经平行 tag 数组 + %r11 指针
# 通道传入（payload 落位逐字不变），被调 $pt 入口序言装载形参 tag；旧入口
# （间接/闭包/函数值调用）清零 %r11 → 形参 tag 全 UNK 探针回退。unknown
# 返回经 %rdx 带 tag（旧约定调用方忽略 rdx）。覆盖：
#   1) 形参 tag → 被调内算术/RC 免探针（int/float/str 混合）；
#   2) 返回 tag → 调用方 RC/算术免探针（含 return 借用值 pick(xs,i)）；
#   3) 8 个 object 形参（栈实参 + tag 数组在出参区共存）；
#   4) 混合类型形参（str/object/int 交错，tag 数组只数 unknown）；
#   5) 递归 object 形参/返回；循环累加器过调用边界；
#   6) 嵌套直调（outer→ident）tag 链；
#   7) 函数值间接调用（fcl = cl）→ 旧入口 UNK 回退，正确性不依赖传播。
# 全部先对拍 CPython 再钉 expect；O0/O1/O2 输出一致。
def addu(a: object, b: object) -> object:
    return a + b
def ident(x: object) -> object:
    return x
def mkfloat(x: float) -> object:
    return x * 2.0
def pick(xs: list[object], i: int) -> object:
    return xs[i]
def many(a: object, b: object, c: object, d: object, e: object, f: object, g: object, h: object) -> object:
    return a + b + c + d + e + f + g + h
def mix(s: str, x: object, n: int) -> object:
    return s + str(x) + str(n)
def recu(n: int, acc: object) -> object:
    if n == 0:
        return acc
    return recu(n - 1, acc + n)
def cl(v: object) -> object:
    return v + 1
print(addu(1, 2.5))
print(addu("ab", "cd"))
print(addu(2.5, 2.5))
print(ident(7))
print(ident("s"))
print(ident(1.25))
print(mkfloat(0.5))
print(mkfloat(1.5) + 1)
xs: list[object] = [10, 2.5, "z"]
print(pick(xs, 0))
print(pick(xs, 1))
print(pick(xs, 2))
print(pick(xs, 1) + 0.5)
print(many(1, 2.5, 3, 4.5, 5, 6.5, 7, 8.5))
print(mix("v=", 3.5, 2))
print(recu(5, 0))
print(recu(4, 0.5))
acc: object = 0
i: int = 0
while i < 6:
    acc = addu(acc, i)
    i = i + 1
print(acc)
def outer(x: object) -> object:
    return ident(x)
print(outer(9.5))
print(outer("q"))
fcl = cl
print(fcl(4))
print(fcl(2.5))
def usecl(x: object) -> object:
    return fcl(x)
print(usecl(3))
print(usecl(0.25))
# --- expect ---
# 3.5
# abcd
# 5.0
# 7
# s
# 1.25
# 1.0
# 4.0
# 10
# 2.5
# z
# 3.0
# 38.0
# v=3.52
# 15
# 10.5
# 15
# 9.5
# q
# 5
# 3.5
# 4
# 1.25
