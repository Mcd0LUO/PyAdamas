# NEW BUG N9 (precision divergence): sum(list[float]) is naive in pyadamas;
# CPython >=3.12 uses Neumaier compensated summation. statistics.mean/fmean
# likewise naive vs CPython exact-Fraction. Diverges on ill-conditioned data.
# CPython: 1.0 / 1.0 / 0.2. pyadamas: 0.9999999999999999 / 0.0 / 0.20000000000000004.
# Not a codegen bug per se (algorithm/version difference) but breaks byte-diff.
# Workaround: math.fsum (correctly rounded, matches CPython) for float sums/means.
import math
import statistics
print("sum:", sum([0.1] * 10), sum([1e16, 1.0, -1e16]))
print("fsum:", math.fsum([0.1] * 10), math.fsum([1e16, 1.0, -1e16]))  # matches
print("mean:", statistics.mean([0.1, 0.2, 0.3]))
