"""a4_unit_convert -- 快速往返型：单位换算表。

温度（C/F/K）、长度（m/ft/in/km/mi）、数据（B/KB/MB/GB）换算，
全部用纯函数计算，固定精度格式化输出换算表与往返一致性检查。
"""
import math


def c_to_f(c: float) -> float:
    return c * 9.0 / 5.0 + 32.0


def f_to_c(f: float) -> float:
    return (f - 32.0) * 5.0 / 9.0


def c_to_k(c: float) -> float:
    return c + 273.15


def m_to_ft(m: float) -> float:
    return m * 3.280839895013123


def ft_to_m(ft: float) -> float:
    return ft / 3.280839895013123


def m_to_mi(m: float) -> float:
    return m / 1609.344


def bytes_to_mb(b: float) -> float:
    return b / 1048576.0


def main() -> None:
    print("== temperature ==")
    temps: list[float] = [-40.0, -10.0, 0.0, 20.0, 37.0, 100.0]
    for c in temps:
        f: float = c_to_f(c)
        k: float = c_to_k(c)
        back: float = f_to_c(f)
        ok: bool = math.isclose(back, c)
        print(format(c, "7.2f"), "C =", format(f, "8.2f"), "F =", format(k, "8.2f"), "K", "roundtrip", ok)
    print("== length ==")
    lengths: list[float] = [0.0, 1.0, 10.0, 100.0, 1000.0, 5000.0]
    for m in lengths:
        ft: float = m_to_ft(m)
        mi: float = m_to_mi(m)
        inch: float = ft * 12.0
        print(format(m, "8.1f"), "m =", format(ft, "10.3f"), "ft =", format(inch, "11.2f"), "in =", format(mi, "8.5f"), "mi")
    print("== data ==")
    sizes: list[float] = [0.0, 1024.0, 1048576.0, 1073741824.0, 5368709120.0]
    for b in sizes:
        kb: float = b / 1024.0
        mb: float = bytes_to_mb(b)
        gb: float = mb / 1024.0
        print(format(b, "14.0f"), "B =", format(kb, "12.2f"), "KB =", format(mb, "10.2f"), "MB =", format(gb, "8.4f"), "GB")
    print("== roundtrip checks ==")
    errs: list[float] = []
    for c in temps:
        errs.append(abs(f_to_c(c_to_f(c)) - c))
    for m in lengths:
        errs.append(abs(ft_to_m(m_to_ft(m)) - m))
    print("max_abs_err", format(math.fsum(errs) / float(len(errs)), ".3e"), "n", len(errs))


if __name__ == "__main__":
    main()
