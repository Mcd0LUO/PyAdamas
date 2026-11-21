"""statistics 模块（pyadamas 自举，分阶段落地）。

阶段一（集中趋势）：mean / fmean / median / median_low / median_high /
median_grouped / mode / multimode。
阶段二（离散度）：pvariance / variance / pstdev / stdev（可选 mu/xbar）。
后续阶段补分位数与二元关系。

精度口径：pa 无 BigInt/Fraction，统一 float + math.fsum 近似；与 CPython
精确有理数中间量的差异见最终模块头（后续 commit 补齐）。
"""

import math


class StatisticsError(ValueError):
    pass


# ── 内部助手 ──────────────────────────────────────────────────────────

def _fmean(data: list[float]) -> float:
    n = len(data)
    if n < 1:
        raise StatisticsError("mean requires at least one data point")
    return math.fsum(data) / n


def _center(data: list[float], mu: object) -> float:
    if mu is None:
        return _fmean(data)
    return float(mu)


def _ss(data: list[float], mu: object) -> float:
    """sum((x - mu)**2)；mu 为 None 时用 fsum 均值。"""
    n = len(data)
    if n < 1:
        return 0.0
    c = _center(data, mu)
    devs: list[float] = []
    for v in data:
        d = v - c
        devs.append(d * d)
    return math.fsum(devs)


def _sqrtprod(x: float, y: float) -> float:
    """sqrt(x*y)，带溢出/下溢缩放（同 CPython _sqrtprod 的思路）。"""
    h = math.sqrt(x * y)
    if not math.isfinite(h):
        if math.isinf(h) and not math.isinf(x) and not math.isinf(y):
            scale = math.pow(2.0, -512.0)
            return _sqrtprod(scale * x, scale * y) / scale
        return h
    if h == 0.0:
        if x != 0.0 and y != 0.0:
            scale = math.pow(2.0, 537.0)
            return _sqrtprod(scale * x, scale * y) / scale
        return h
    d = x * y - h * h
    return h + d / (2.0 * h)


def _rank(data: list[float], start: float) -> list[float]:
    """平均并列秩（CPython _rank 的 ties='average' 口径）。"""
    n = len(data)
    out: list[float] = []
    for i in range(n):
        v = data[i]
        less = 0
        equal = 0
        for j in range(n):
            if data[j] < v:
                less += 1
            elif data[j] == v:
                equal += 1
        rank = start - 1.0 + less + (equal + 1.0) / 2.0
        out.append(rank)
    return out


# ── 集中趋势 ──────────────────────────────────────────────────────────

def mean(data: list[float]) -> float:
    """算术平均（float 口径，见模块头的精度说明）。"""
    n = len(data)
    if n < 1:
        raise StatisticsError("mean requires at least one data point")
    return math.fsum(data) / n


def fmean(data: list[float]) -> float:
    """快速浮点平均（等价于 mean 的 float 路径）。"""
    return _fmean(data)


def _sorted(data: list[float]) -> list[float]:
    if len(data) < 1:
        raise StatisticsError("no median for empty data")
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


def median_grouped(data: list[float], interval: float = 1.0) -> float:
    """分组数据中位数；interval 是组距，默认 1.0。"""
    xs = sorted(data)
    n = len(xs)
    if n == 0:
        raise StatisticsError("no median for empty data")
    x = xs[n // 2]
    iv = float(interval)
    xf = float(x)
    i = 0
    while i < n and xs[i] < x:
        i += 1
    j = i
    while j < n and xs[j] == x:
        j += 1
    L = xf - iv / 2.0
    cf = i
    f = j - i
    return L + iv * (n / 2.0 - cf) / f


def mode(data: list[int]) -> int:
    """众数：并列时返回最先出现的值（CPython 3.14 口径）。"""
    n = len(data)
    if n == 0:
        raise StatisticsError("no mode for empty data")
    best = data[0]
    best_count = 0
    for v in data:
        c = 0
        for w in data:
            if w == v:
                c += 1
        if c > best_count:
            best = v
            best_count = c
    return best


def multimode(data: list[int]) -> list[int]:
    """所有众数，按首次出现顺序返回；空数据返回空列表。"""
    n = len(data)
    out: list[int] = []
    if n == 0:
        return out
    values: list[int] = []
    counts: list[int] = []
    for v in data:
        k = 0
        found = -1
        while k < len(values):
            if values[k] == v:
                found = k
                break
            k += 1
        if found < 0:
            values.append(v)
            counts.append(1)
        else:
            counts[found] = counts[found] + 1
    best = 0
    for c in counts:
        if c > best:
            best = c
    k = 0
    while k < len(values):
        if counts[k] == best:
            out.append(values[k])
        k += 1
    return out


# ── 离散度 ────────────────────────────────────────────────────────────

def pvariance(data: list[float], mu: object = None) -> float:
    """总体方差；可选 mu 参数避免重复求均值。"""
    n = len(data)
    if n < 1:
        raise StatisticsError("pvariance requires at least one data point")
    return _ss(data, mu) / n


def variance(data: list[float], xbar: object = None) -> float:
    """样本方差（n-1 自由度）；可选 xbar 参数。"""
    n = len(data)
    if n < 2:
        raise StatisticsError("variance requires at least two data points")
    return _ss(data, xbar) / (n - 1)


def pstdev(data: list[float], mu: object = None) -> float:
    return math.sqrt(pvariance(data, mu))


def stdev(data: list[float], xbar: object = None) -> float:
    return math.sqrt(variance(data, xbar))
