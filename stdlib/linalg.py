"""BLAS 行主序矩阵运算封装。"""

cimport cblas


def matmul(a: list[float], b: list[float], m: int, n: int, k: int) -> list[float]:
    """计算行主序矩阵 A(m×k) 与 B(k×n) 的乘积，返回长度 m*n 的 list。"""
    if len(a) < m * k:
        raise ValueError("a has fewer than m*k elements")
    if len(b) < k * n:
        raise ValueError("b has fewer than k*n elements")
    c: list[float] = []
    i = 0
    while i < m * n:
        c.append(0.0)
        i += 1
    cblas.dgemm(cblas.ROW_MAJOR, cblas.NO_TRANS, cblas.NO_TRANS,
                m, n, k, 1.0, a, k, b, n, 0.0, c, n)
    return c


def dot(x: list[float], y: list[float]) -> float:
    """计算两个等长向量的点积。"""
    if len(x) != len(y):
        raise ValueError("vectors must have the same length")
    return cblas.ddot(len(x), x, 1, y, 1)
