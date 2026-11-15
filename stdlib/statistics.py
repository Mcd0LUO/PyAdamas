"""statistics 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

覆盖 mean / fmean / median / median_low / median_high / mode /
pvariance / variance / pstdev / stdev / harmonic_mean / geometric_mean /
quantiles 的常用行为，输入为 list[float]（mode 为 list[int]）。
"""

import math


class StatisticsError(ValueError):
    pass


def _check(data: list[float]) -> int:
    n = len(data)
    if n == 0:
        raise StatisticsError("no data")
    return n


def mean(data: list[float]) -> float:
    n = _check(data)
    total = 0.0
    for v in data:
        total += v
    return total / n


def fmean(data: list[float]) -> float:
    return mean(data)


def _sorted(data: list[float]) -> list[float]:
    _check(data)
    return sorted(data)


def median(data: list[float]) -> float:
    xs = _sorted(data)
    n = len(xs)
    mid = n // 2
    if n % 2 == 1:
        return xs[mid]
    return (xs[mid - 1] + xs[mid]) / 2.0


def median_low(data: list[float]) -> float:
    xs = _sorted(data)
    return xs[(len(xs) - 1) // 2]


def median_high(data: list[float]) -> float:
    xs = _sorted(data)
    return xs[len(xs) // 2]


def mode(data: list[int]) -> int:
    n = len(data)
    if n == 0:
        raise StatisticsError("no data")
    best = data[0]
    best_count = 0
    for v in data:
        count = 0
        for w in data:
            if w == v:
                count += 1
        if count > best_count:
            best = v
            best_count = count
    return best


def pvariance(data: list[float]) -> float:
    n = _check(data)
    mu = mean(data)
    total = 0.0
    for v in data:
        d = v - mu
        total += d * d
    return total / n


def variance(data: list[float]) -> float:
    n = _check(data)
    if n < 2:
        raise StatisticsError("variance requires at least two data points")
    mu = mean(data)
    total = 0.0
    for v in data:
        d = v - mu
        total += d * d
    return total / (n - 1)


def pstdev(data: list[float]) -> float:
    return math.sqrt(pvariance(data))


def stdev(data: list[float]) -> float:
    return math.sqrt(variance(data))


def harmonic_mean(data: list[float]) -> float:
    n = _check(data)
    total = 0.0
    for v in data:
        if v <= 0.0:
            raise StatisticsError("harmonic mean requires positive values")
        total += 1.0 / v
    return n / total


def geometric_mean(data: list[float]) -> float:
    n = _check(data)
    total = 0.0
    for v in data:
        if v <= 0.0:
            raise StatisticsError("geometric mean requires positive values")
        total += math.log(v)
    return math.exp(total / n)


def quantiles(data: list[float], n: int = 4,
              method: str = "exclusive") -> list[float]:
    """把数据分成 n 个等概率区间，返回 n-1 个切分点。

    method="exclusive"（默认，样本分位数）与 "inclusive"（总体分位数）
    的插值公式与 CPython 一致；其它 method 抛 StatisticsError。
    """
    if n < 1:
        raise StatisticsError("n must be at least 1")
    xs = sorted(data)
    if len(xs) < 2:
        raise StatisticsError("must have at least two data points")
    out: list[float] = []
    if method == "exclusive":
        m = len(xs) + 1
        i = 1
        while i < n:
            j = i * m // n
            delta = i * m - j * n
            out.append((xs[j - 1] * (n - delta) + xs[j] * delta) / n)
            i += 1
        return out
    if method == "inclusive":
        m = len(xs) - 1
        i = 1
        while i < n:
            j = i * m // n
            delta = i * m - j * n
            out.append((xs[j] * (n - delta) + xs[j + 1] * delta) / n)
            i += 1
        return out
    raise StatisticsError("method must be exclusive or inclusive")
