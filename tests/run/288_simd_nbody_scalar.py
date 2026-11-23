# pyadamas: simd
# SIMD nbody 标量 fallback / 边界回归（0.334）：守卫不满足整段走标量，异常时机不变。
# 期望输出 flag-on==off==O0==O1。d2=0 case  pyadamas 浮点除零得 inf/nan（divsd 语义），
# CPython 抛 ZeroDivisionError —— 设计口径差异（docs §10），不与 CPython 对拍该 case。
import math


# 0.426：int(NaN)/int(inf) 现在按 CPython 语义抛异常（以前是 UB 的
# 哨兵垃圾值，这个用例的 expect 曾把垃圾值钉住）。这里把「非有限/越界」
# 统一记成 -1，避免依赖 UB；其余仍是精确的 int(x*1e9)。
def i9(x: float) -> int:
    try:
        return int(x * 1e9)
    except ValueError:
        return -1
    except OverflowError:
        return -1

def advance(b: list[float], dt: float, steps: int, nb: int) -> None:
    for _ in range(steps):
        for i in range(nb):
            for j in range(i + 1, nb):
                dx = b[i * 7] - b[j * 7]
                dy = b[i * 7 + 1] - b[j * 7 + 1]
                dz = b[i * 7 + 2] - b[j * 7 + 2]
                d2 = dx * dx + dy * dy + dz * dz
                mag = dt / (d2 * math.sqrt(d2))
                b[i * 7 + 3] -= dx * b[j * 7 + 6] * mag
                b[i * 7 + 4] -= dy * b[j * 7 + 6] * mag
                b[i * 7 + 5] -= dz * b[j * 7 + 6] * mag
                b[j * 7 + 3] += dx * b[i * 7 + 6] * mag
                b[j * 7 + 4] += dy * b[i * 7 + 6] * mag
                b[j * 7 + 5] += dz * b[i * 7 + 6] * mag
        for i in range(nb):
            b[i * 7] += dt * b[i * 7 + 3]
            b[i * 7 + 1] += dt * b[i * 7 + 4]
            b[i * 7 + 2] += dt * b[i * 7 + 5]

def advanceu(b, dt: float, steps: int, nb: int) -> None:
    for _ in range(steps):
        for i in range(nb):
            for j in range(i + 1, nb):
                dx = b[i * 7] - b[j * 7]
                dy = b[i * 7 + 1] - b[j * 7 + 1]
                dz = b[i * 7 + 2] - b[j * 7 + 2]
                d2 = dx * dx + dy * dy + dz * dz
                mag = dt / (d2 * math.sqrt(d2))
                b[i * 7 + 3] -= dx * b[j * 7 + 6] * mag
                b[i * 7 + 4] -= dy * b[j * 7 + 6] * mag
                b[i * 7 + 5] -= dz * b[j * 7 + 6] * mag
                b[j * 7 + 3] += dx * b[i * 7 + 6] * mag
                b[j * 7 + 4] += dy * b[i * 7 + 6] * mag
                b[j * 7 + 5] += dz * b[i * 7 + 6] * mag
        for i in range(nb):
            b[i * 7] += dt * b[i * 7 + 3]
            b[i * 7 + 1] += dt * b[i * 7 + 4]
            b[i * 7 + 2] += dt * b[i * 7 + 5]

# 1) len < 7n → 范围守卫 fallback，IndexError 在第一个越界访问处抛（时机钉住）
b = []
for _ in range(34):
    b.append(0.0)
b[6] = 39.47841760435743
b[7] = 4.841431442464721
b[13] = 8.34336671824458
b[19] = 12.894369562139131
b[25] = 15.379697114850917
try:
    advance(b, 0.01, 1, 5)
except IndexError:
    print('idx', i9(b[3]), i9(b[10]), i9(b[31]))
# 2) kind 混合（b 含 int 元素）→ kind!=0 守卫 fallback
c = []
for _ in range(14):
    c.append(0.0)
c[6] = 39.47841760435743
c[7] = 4.841431442464721
c[13] = 8.34336671824458
c[8] = 1
advanceu(c, 0.01, 3, 2)
print('kind', i9(c[7]), i9(c[10]))
# 3) 重合体 d2=0 → mag=inf → nan 传播（divsd 语义，on==off 钉住）
d = []
for _ in range(14):
    d.append(0.0)
d[6] = 39.47841760435743
d[13] = 8.34336671824458
advance(d, 0.01, 1, 2)
print('zero', d[3] != d[3], d[10] != d[10])
# 4) 负 dt：向量路径正常（mag 符号翻转）
e = []
for _ in range(14):
    e.append(0.0)
e[6] = 39.47841760435743
e[7] = 4.841431442464721
e[13] = 8.34336671824458
e[8] = 4.124798564124305
advance(e, -0.01, 5, 2)
print('negdt', i9(e[7]), i9(e[10]))

# --- expect ---
# idx -1 -16842717 0
# kind 4840482202 -47463882
# zero True True
# negdt 4840317072 37147981
