"""bisect 模块的 pyadamas 子集（纯 pyadamas 源码）。

当前只覆盖有序 int 列表；支持 bisect_left / bisect_right / bisect /
insort_left / insort_right / insort。hi 省略或 0 表示列表末尾。

list[float] 扩展尝试结论（2026-11-23）：**做不到**。见 docs/stdlib-gaps.md
第 8 节：共享模块的函数签名要么是 list[int]（拒绝 float 实参），要么是
list[unknown]/裸 list（fresh_hole 被首个调用点定型，混合 int/float 调用时
float 元素读出乱码）。只有改成编译期泛型（需动 src/）才能同时支持两者；
本次保持 list[int] 不动，不破坏已有 API。
"""


def bisect_left(a: list[int], x: int, lo: int = 0, hi: int = 0) -> int:
    if hi <= 0 or hi > len(a):
        hi = len(a)
    while lo < hi:
        mid = (lo + hi) // 2
        if a[mid] < x:
            lo = mid + 1
        else:
            hi = mid
    return lo


def bisect_right(a: list[int], x: int, lo: int = 0, hi: int = 0) -> int:
    if hi <= 0 or hi > len(a):
        hi = len(a)
    while lo < hi:
        mid = (lo + hi) // 2
        if x < a[mid]:
            hi = mid
        else:
            lo = mid + 1
    return lo


def bisect(a: list[int], x: int, lo: int = 0, hi: int = 0) -> int:
    return bisect_right(a, x, lo, hi)


def insort_left(a: list[int], x: int, lo: int = 0, hi: int = 0) -> None:
    a.insert(bisect_left(a, x, lo, hi), x)


def insort_right(a: list[int], x: int, lo: int = 0, hi: int = 0) -> None:
    a.insert(bisect_right(a, x, lo, hi), x)


def insort(a: list[int], x: int, lo: int = 0, hi: int = 0) -> None:
    insort_right(a, x, lo, hi)
