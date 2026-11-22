# 散射写（stride 写，coef>1）：`out[j*2] = e` 的写地址跨步 2 个元素，x86 没有
# packed scatter → 逐 lane 两条 movsd（低 lane 直接存、高 lane movhlps 抽出来再存）。
# 计算与装载仍被向量化，只有写退化成 2 条标量（阶段 B5 阶段 3）。
#
# 覆盖：coef=2/3、n=0/1/2/3/5、float 元素、与 map/reduction 混用、
# 以及越界 IndexError（守卫失败 → 标量尾，异常时机不变）。


def stride2(out: list[float], a: list[float], n: int) -> None:
    for j in range(n):
        out[j * 2] = a[j] + 1.0


def stride3(out: list[float], a: list[float], n: int) -> None:
    for j in range(n):
        out[j * 3] = a[j] * 2.0 - 0.5


def stride_mix(out: list[float], a: list[float], s: list[float], n: int) -> None:
    for j in range(n):
        out[j * 2] = a[j] + 1.0
        s[0] += a[j]


def run(n: int) -> None:
    a = []
    for j in range(8):
        a.append(float(j) - 3.0)
    o2 = [0.0] * 20
    stride2(o2, a, n)
    print("s2", n, o2[0], o2[1], o2[2], o2[8], o2[9])
    o3 = [0.0] * 30
    stride3(o3, a, n)
    print("s3", n, o3[0], o3[3], o3[6], o3[12], o3[13])
    o4 = [0.0] * 20
    sm = [0.0]
    stride_mix(o4, a, sm, n)
    print("sx", n, sm[0], o4[0], o4[2], o4[8])


for n in (0, 1, 2, 3, 5, 8):
    run(n)

# 越界：out 只有 4 个元素，j*2 到 j=2 就越界 → IndexError
try:
    o = [0.0] * 4
    stride2(o, [1.0, 2.0, 3.0], 3)
    print("no raise")
except IndexError:
    print("IndexError")

# --- expect ---
# s2 0 0.0 0.0 0.0 0.0 0.0
# s3 0 0.0 0.0 0.0 0.0 0.0
# sx 0 0.0 0.0 0.0 0.0
# s2 1 -2.0 0.0 0.0 0.0 0.0
# s3 1 -6.5 0.0 0.0 0.0 0.0
# sx 1 -3.0 -2.0 0.0 0.0
# s2 2 -2.0 0.0 -1.0 0.0 0.0
# s3 2 -6.5 -4.5 0.0 0.0 0.0
# sx 2 -5.0 -2.0 -1.0 0.0
# s2 3 -2.0 0.0 -1.0 0.0 0.0
# s3 3 -6.5 -4.5 -2.5 0.0 0.0
# sx 3 -6.0 -2.0 -1.0 0.0
# s2 5 -2.0 0.0 -1.0 2.0 0.0
# s3 5 -6.5 -4.5 -2.5 1.5 0.0
# sx 5 -5.0 -2.0 -1.0 2.0
# s2 8 -2.0 0.0 -1.0 2.0 0.0
# s3 8 -6.5 -4.5 -2.5 1.5 0.0
# sx 8 4.0 -2.0 -1.0 2.0
# IndexError
