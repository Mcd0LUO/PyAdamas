def lcg(state: int) -> int:
    # int64-safe LCG (Numerical Recipes): state*mult < 2^63, no overflow wrap
    return (state * 1664525 + 1013904223) % 4294967296


"""d2_stats -- 数值算子型：描述统计与百分位。

LCG 生成样本，math.fsum 算均值/方差/偏度（正确舍入，两边一致），
排序后线性插值百分位，直方图分桶，并用 statistics.median/quantiles
交叉验证。避免 sum(float) 与 statistics.mean 的精度差异。
"""
import math
import statistics


def gen_samples(n: int, seed: int) -> list[float]:
    xs: list[float] = []
    state: int = seed
    for i in range(n):
        state = lcg(state)
        u1: float = float(state % 1000000 + 1) / 1000001.0
        state = lcg(state)
        u2: float = float(state % 1000000 + 1) / 1000001.0
        # sum of uniforms -> approximately normal, deterministic
        xs.append((u1 + u2 - 1.0) * 10.0)
    return xs


def mean(xs: list[float]) -> float:
    return math.fsum(xs) / float(len(xs))


def variance(xs: list[float], m: float) -> float:
    sq: list[float] = []
    for x in xs:
        d: float = x - m
        sq.append(d * d)
    return math.fsum(sq) / float(len(xs) - 1)


def skewness(xs: list[float], m: float, sd: float) -> float:
    cube: list[float] = []
    for x in xs:
        d: float = (x - m) / sd
        cube.append(d * d * d)
    return math.fsum(cube) / float(len(xs))


def percentile(sorted_xs: list[float], p: float) -> float:
    n: int = len(sorted_xs)
    if n == 1:
        return sorted_xs[0]
    rank: float = p / 100.0 * float(n - 1)
    lo: int = int(math.floor(rank))
    hi: int = lo + 1
    if hi >= n:
        return sorted_xs[n - 1]
    frac: float = rank - float(lo)
    return sorted_xs[lo] + (sorted_xs[hi] - sorted_xs[lo]) * frac


def main() -> None:
    xs: list[float] = gen_samples(5000, 424242)
    m: float = mean(xs)
    var: float = variance(xs, m)
    sd: float = math.sqrt(var)
    sk: float = skewness(xs, m, sd)
    print("n", len(xs))
    print("mean", format(m, ".10f"))
    print("variance", format(var, ".10f"))
    print("stdev", format(sd, ".10f"))
    print("skewness", format(sk, ".10f"))
    srt: list[float] = sorted(xs)
    print("min", format(srt[0], ".6f"), "max", format(srt[len(srt) - 1], ".6f"))
    print("-- percentiles --")
    for p in [5.0, 25.0, 50.0, 75.0, 95.0]:
        print(format(p, "5.1f"), format(percentile(srt, p), ".10f"))
    print("-- statistics cross-check --")
    print("median", format(statistics.median(xs), ".10f"))
    q: list[float] = statistics.quantiles(xs, n=4)
    print("quartiles", format(q[0], ".6f"), format(q[1], ".6f"), format(q[2], ".6f"))
    print("-- histogram --")
    buckets: list[int] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    for x in xs:
        idx: int = int(math.floor((x + 10.0) / 2.0))
        if idx < 0:
            idx = 0
        if idx > 9:
            idx = 9
        buckets[idx] += 1
    for i in range(10):
        lo: float = -10.0 + float(i) * 2.0
        print(format(lo, "6.1f"), buckets[i])


if __name__ == "__main__":
    main()
