# pyadamas: vect
# Convert i64→f64 的「不变量 + j」回归（阶段 B1）：
#   * float(M + j)（M = 循环不变量 int：函数参数 / 外层计算 / 全局）走
#     VecI→VecF 折算（不变量广播 + paddq + cvtdq2pd），以前只有 j+常量 的
#     j_src 快速路径能向量化，这里钉住通用路径；
#   * 编译期常量 M 超出 leaq disp32 可编码范围（> kAffMaxOff）时必须回退
#     通用 VecI 折算，不能生成超范围位移（旧缺陷：汇编器报
#     "0x... out of range of signed 32bit displacement" 直接编译失败）。
# 每段独立函数，避免同名变量/参数跨段冲突；输出对拍 CPython。

GM = 11


def _case_param(m: int) -> None:
    a = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5]
    out = [0.0] * 8
    for j in range(8):
        out[j] = float(m + j) * a[j]
    print(out)


def _case_outer(k: int) -> None:
    m = k * 4 - 7
    a = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5]
    out = [0.0] * 8
    for j in range(8):
        out[j] = float(m + j) * a[j]
    print(out)


def _case_global() -> None:
    out = [0.0] * 8
    for j in range(8):
        out[j] = float(GM + j)
    print(out)


def _case_big_const() -> None:
    # 2^40：常量不变量 > disp32；必须走 VecI 广播折算（不再发超范围 leaq）。
    a = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5]
    out = [0.0] * 8
    for j in range(8):
        out[j] = float(1099511627776 + j) * a[j]
    print(out)


def _case_big_const_32() -> None:
    # 3e9：> INT32_MAX 的常量不变量（同样走通用折算）。
    out = [0.0] * 8
    for j in range(8):
        out[j] = float(3000000000 + j)
    print(out)


def _case_negative(m: int) -> None:
    a = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5]
    out = [0.0] * 8
    for j in range(8):
        out[j] = float(m - j) * a[j]
    print(out)


def _case_reduce(m: int) -> None:
    s = 0.0
    for j in range(8):
        s = s + float(m + j)
    print(s)


def _case_multi_use(m: int) -> None:
    # 同一个 VecI（m+j）被两个 Convert 使用，钉住重复使用不串寄存器。
    a = [0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5]
    out = [0.0] * 8
    out2 = [0.0] * 8
    for j in range(8):
        out[j] = float(m + j) * a[j]
        out2[j] = float(m + j) + a[j]
    print(out)
    print(out2)


_case_param(3)
_case_outer(2)
_case_global()
_case_big_const()
_case_big_const_32()
_case_negative(5)
_case_reduce(3)
_case_multi_use(3)

# --- expect ---
# [1.5, 6.0, 12.5, 21.0, 31.5, 44.0, 58.5, 75.0]
# [0.5, 3.0, 7.5, 14.0, 22.5, 33.0, 45.5, 60.0]
# [11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0]
# [549755813888.0, 1649267441665.5, 2748779069445.0, 3848290697226.5, 4947802325010.0, 6047313952795.5, 7146825580583.0, 8246337208372.5]
# [3000000000.0, 3000000001.0, 3000000002.0, 3000000003.0, 3000000004.0, 3000000005.0, 3000000006.0, 3000000007.0]
# [2.5, 6.0, 7.5, 7.0, 4.5, 0.0, -6.5, -15.0]
# 52.0
# [1.5, 6.0, 12.5, 21.0, 31.5, 44.0, 58.5, 75.0]
# [3.5, 5.5, 7.5, 9.5, 11.5, 13.5, 15.5, 17.5]
