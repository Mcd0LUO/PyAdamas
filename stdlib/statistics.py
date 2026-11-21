"""statistics 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

覆盖 CPython 3.14 statistics 的 tractable 核心：
- 集中趋势：mean / fmean / geometric_mean / harmonic_mean /
  median / median_low / median_high / median_grouped / mode / multimode
- 离散度：pvariance / variance / pstdev / stdev（均支持可选 mu/xbar）
- 分位数：quantiles（inclusive / exclusive，n 分位）
- 二元关系：covariance / correlation（linear / ranked）/ linear_regression

异常：StatisticsError(ValueError)。

== 浮点精度口径（重要）==
CPython 的 mean/variance/covariance 等用 Fraction 做**精确有理数中间量**，
最后一次除法才舍入；pyadamas 没有 BigInt/Fraction，这里统一用 float +
math.fsum 近似（fsum 保证求和正确舍入，但"先求和再除"与"精确和有理性
最后舍入"仍可能差 1 ulp，方差类还有均值先舍入带来的二阶误差）。

钉住的已知差异（tests/run/380-385 覆盖）：
- mean([0.1, 0.2, 0.3])：本模块 0.19999999999999998（fsum 先把和舍入成
  0.6 再除），CPython 精确和有理性最后舍入得 0.2。这是典型的 double
  rounding 差异；用 fsum 无法消除（需要精确有理数和）。
- pvariance([1e50, 1, -1e50])：本模块 6.666666666666668e+99，CPython
  6.666666666666667e+99（差 1 ulp）；同数据的 variance 恰好逐位一致。
- variance([0.1, 0.2, 0.3]) / pvariance 同数据与 CPython 逐位一致（实测）。
- harmonic_mean([40, 60])：本模块 47.99999999999999，CPython 48.0；
  CPython 对 1/x 做精确有理数求和，本模块对已舍入的 1/x 做 fsum。
- mean/fmean 对 [1e50, 1, -1e50] 因 fsum 正确舍入，与 CPython 同为 1/3。
- median_grouped 的插值在 double 内与 CPython 完全相同（同一公式与运算序）。
- quantiles 的插值 CPython 本身也是 double 运算（同一公式/运算序），逐位一致。
- geometric_mean 与 CPython 3.14 同用 fsum(log)+exp，逐位一致（含 inf/nan
  的显式分支：pa 的 math.fsum([0.0, inf]) 返回 nan，故本模块先扫 inf）。

== 与 CPython 的类型差异 ==
- mean 在 CPython 中当数据全为 int 且整除时返回 int；本模块形参/返回统一
  list[float] -> float，恒返回 float（2.0 而非 2）。
- median/median_low/median_high 对奇数个 int 数据 CPython 返回 int，本模块
  返回 float（3.0 而非 3）。
- pvariance/pstdev 单点数据 CPython 返回 int 0，本模块返回 0.0。
- harmonic_mean 遇到 0 时 CPython 返回 int 0，本模块返回 0.0。
- mode/multimode 只接受 list[int]（pa 的 list[object] 对 float 元素表示有
  已知缺陷，会打印成 int 位模式；字符串列表也无法用 list[object] 安全往返）。
  CPython 的任意可哈希序列面不支持。
- linear_regression 返回 LinearRegression 类（slope/intercept 字段 + 与
  CPython 相同的 repr），不是 namedtuple：不支持元组解包/索引（pa 用户类
  的 __iter__ 解包会段错误，已单独记录）。
- fmean 的 weights= 参数不支持（传了是 TypeError）。
"""

import math


class StatisticsError(ValueError):
    pass


class LinearRegression:
    """linear_regression 的返回值：slope/intercept 两个字段。"""

    slope: float
    intercept: float

    def __init__(self, slope: float, intercept: float) -> None:
        self.slope = slope
        self.intercept = intercept

    def __repr__(self) -> str:
        return ("LinearRegression(slope=" + str(self.slope)
                + ", intercept=" + str(self.intercept) + ")")


# ── 内部助手 ──────────────────────────────────────────────────────────

def _fmean(data: list[float]) -> float:
    n = len(data)
    if n < 1:
        raise StatisticsError("mean requires at least one data point")
    pos_inf = False
    neg_inf = False
    has_nan = False
    for v in data:
        if math.isnan(v):
            has_nan = True
        elif math.isinf(v):
            if v > 0.0:
                pos_inf = True
            else:
                neg_inf = True
    if has_nan:
        return math.nan
    if pos_inf and neg_inf:
        raise ValueError("-inf + inf in fsum")
    if pos_inf:
        return math.inf
    if neg_inf:
        return -math.inf
    return math.fsum(data) / n


def _center(data: list[float], mu: object) -> float:
    if mu is None:
        return _fmean(data)
    return float(mu)


def _ss(data: list[float], mu: object) -> float:
    """sum((x - mu)**2)；mu 为 None 时用 fsum 均值。

    含 nan/inf 时按 CPython _ss 的特殊值分支：mu 省略时返回非有限值的
    和（inf/-inf/nan 的传播），mu 显式给出时返回 (x-mu)**2 的和。
    """
    n = len(data)
    if n < 1:
        return 0.0
    special = 0.0
    has_special = False
    for v in data:
        if math.isnan(v) or math.isinf(v):
            has_special = True
            if mu is None:
                special = special + v
            else:
                d = v - float(mu)
                special = special + d * d
    if has_special:
        return special
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
    """算术平均（float 口径，见模块头的精度说明）。

    特殊值按 CPython 口径：nan 传染；+inf/-inf 单独出现时返回对应 inf；
    两者同时出现返回 nan（CPython mean 用精确和，fmean 走 fsum 会抛
    ValueError，这里两种口径分开实现）。
    """
    n = len(data)
    if n < 1:
        raise StatisticsError("mean requires at least one data point")
    pos_inf = False
    neg_inf = False
    has_nan = False
    for v in data:
        if math.isnan(v):
            has_nan = True
        elif math.isinf(v):
            if v > 0.0:
                pos_inf = True
            else:
                neg_inf = True
    if has_nan:
        return math.nan
    if pos_inf and neg_inf:
        return math.nan
    if pos_inf:
        return math.inf
    if neg_inf:
        return -math.inf
    return math.fsum(data) / n


def fmean(data: list[float]) -> float:
    """快速浮点平均（等价于 mean 的 float 路径）。"""
    return _fmean(data)


def geometric_mean(data: list[float]) -> float:
    """几何平均；允许 0（返回 0.0），负数抛 StatisticsError，nan/inf 传染。"""
    n = 0
    found_zero = False
    found_nan = False
    found_inf = False
    logs: list[float] = []
    for v in data:
        n += 1
        if math.isnan(v):
            found_nan = True
        elif math.isinf(v):
            if v > 0.0:
                found_inf = True
            else:
                raise StatisticsError("No negative inputs allowed")
        elif v > 0.0:
            logs.append(math.log(v))
        elif v == 0.0:
            found_zero = True
        else:
            raise StatisticsError("No negative inputs allowed")
    if n < 1:
        raise StatisticsError("Must have a non-empty dataset")
    if found_nan:
        return math.nan
    if found_inf:
        if found_zero:
            return math.nan
        return math.inf
    total = math.fsum(logs)
    if math.isnan(total):
        return math.nan
    if found_zero:
        return 0.0
    return math.exp(total / n)


def harmonic_mean(data: list[float]) -> float:
    """调和平均；允许 0（整体返回 0.0），负数抛 StatisticsError。"""
    n = len(data)
    if n < 1:
        raise StatisticsError("harmonic_mean requires at least one data point")
    if n == 1:
        x = data[0]
        if x < 0.0:
            raise StatisticsError("harmonic mean does not support negative values")
        return x
    inv: list[float] = []
    for v in data:
        if v < 0.0:
            raise StatisticsError("harmonic mean does not support negative values")
        if v == 0.0:
            return 0.0
        inv.append(1.0 / v)
    total = math.fsum(inv)
    if total <= 0.0:
        raise StatisticsError("Weighted sum must be positive")
    return float(n) / total


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
    mss = pvariance(data, mu)
    if math.isnan(mss) or math.isinf(mss):
        raise ValueError("inf or nan encountered in data")
    return math.sqrt(mss)


def stdev(data: list[float], xbar: object = None) -> float:
    mss = variance(data, xbar)
    if math.isnan(mss) or math.isinf(mss):
        raise ValueError("inf or nan encountered in data")
    return math.sqrt(mss)


# ── 分位数 ────────────────────────────────────────────────────────────

def quantiles(data: list[float], n: int = 4,
              method: str = "exclusive") -> list[float]:
    """把数据分成 n 个等概率区间，返回 n-1 个切分点。

    method="exclusive"（默认，样本分位数）/ "inclusive"（总体分位数），
    与 CPython 3.14 同公式（exclusive 的 j 钳制到 1..ld-1）；数据只有
    1 个点时返回 n-1 个该值；空数据抛 StatisticsError；未知 method 抛
    ValueError（CPython 3.14 同口径）。
    """
    if n < 1:
        raise StatisticsError("n must be at least 1")
    xs = sorted(data)
    ld = len(xs)
    if ld < 2:
        if ld == 1:
            out1: list[float] = []
            i = 0
            while i < n - 1:
                out1.append(xs[0])
                i += 1
            return out1
        raise StatisticsError("must have at least one data point")
    if method == "inclusive":
        m = ld - 1
        out: list[float] = []
        i = 1
        while i < n:
            j = i * m // n
            delta = i * m - j * n
            out.append((xs[j] * (n - delta) + xs[j + 1] * delta) / n)
            i += 1
        return out
    if method == "exclusive":
        m = ld + 1
        out2: list[float] = []
        i = 1
        while i < n:
            j = i * m // n
            if j < 1:
                j = 1
            if j > ld - 1:
                j = ld - 1
            delta = i * m - j * n
            out2.append((xs[j - 1] * (n - delta) + xs[j] * delta) / n)
            i += 1
        return out2
    raise ValueError("Unknown method: '" + method + "'")


# ── 二元关系 ──────────────────────────────────────────────────────────

def covariance(x: list[float], y: list[float]) -> float:
    n = len(x)
    if len(y) != n:
        raise StatisticsError(
            "covariance requires that both inputs have same number of data points")
    if n < 2:
        raise StatisticsError("covariance requires at least two data points")
    xbar = math.fsum(x) / n
    ybar = math.fsum(y) / n
    prods: list[float] = []
    for i in range(n):
        prods.append((x[i] - xbar) * (y[i] - ybar))
    return math.fsum(prods) / (n - 1)


def _pearson(x: list[float], y: list[float]) -> float:
    n = len(x)
    xbar = math.fsum(x) / n
    ybar = math.fsum(y) / n
    xs: list[float] = []
    ys: list[float] = []
    for v in x:
        xs.append(v - xbar)
    for v in y:
        ys.append(v - ybar)
    sxy = math.fsum([xs[i] * ys[i] for i in range(n)])
    sxx = math.fsum([xs[i] * xs[i] for i in range(n)])
    syy = math.fsum([ys[i] * ys[i] for i in range(n)])
    den = _sqrtprod(sxx, syy)
    if den == 0.0:
        raise StatisticsError("at least one of the inputs is constant")
    return sxy / den


def _spearman(x: list[float], y: list[float]) -> float:
    n = len(x)
    start = (n - 1) / -2.0
    xs = _rank(x, start)
    ys = _rank(y, start)
    sxy = math.fsum([xs[i] * ys[i] for i in range(n)])
    sxx = math.fsum([xs[i] * xs[i] for i in range(n)])
    syy = math.fsum([ys[i] * ys[i] for i in range(n)])
    den = _sqrtprod(sxx, syy)
    if den == 0.0:
        raise StatisticsError("at least one of the inputs is constant")
    return sxy / den


def correlation(x: list[float], y: list[float], *,
                method: str = "linear") -> float:
    """Pearson（method="linear"）或 Spearman（method="ranked"）相关系数。"""
    n = len(x)
    if len(y) != n:
        raise StatisticsError(
            "correlation requires that both inputs have same number of data points")
    if n < 2:
        raise StatisticsError("correlation requires at least two data points")
    if method == "linear":
        return _pearson(x, y)
    if method == "ranked":
        return _spearman(x, y)
    raise ValueError("Unknown method: '" + method + "'")


def linear_regression(x: list[float], y: list[float], *,
                      proportional: bool = False) -> LinearRegression:
    """最小二乘直线 y = slope*x + intercept。"""
    n = len(x)
    if len(y) != n:
        raise StatisticsError(
            "linear regression requires that both inputs have same number of data points")
    if n < 2:
        raise StatisticsError("linear regression requires at least two data points")
    if proportional:
        sxy = math.fsum([x[i] * y[i] for i in range(n)]) + 0.0
        sxx = math.fsum([x[i] * x[i] for i in range(n)])
        if sxx == 0.0:
            raise StatisticsError("x is constant")
        return LinearRegression(sxy / sxx, 0.0)
    xbar = math.fsum(x) / n
    ybar = math.fsum(y) / n
    dx: list[float] = []
    dy: list[float] = []
    for v in x:
        dx.append(v - xbar)
    for v in y:
        dy.append(v - ybar)
    sxy = math.fsum([dx[i] * dy[i] for i in range(n)]) + 0.0
    sxx = math.fsum([dx[i] * dx[i] for i in range(n)])
    if sxx == 0.0:
        raise StatisticsError("x is constant")
    slope = sxy / sxx
    intercept = ybar - slope * xbar
    return LinearRegression(slope, intercept)
