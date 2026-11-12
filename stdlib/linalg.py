"""BLAS 行主序矩阵运算封装。"""

cimport cblas


def _zeros(n: int) -> list[float]:
    out: list[float] = []
    i = 0
    while i < n:
        out.append(0.0)
        i += 1
    return out


def _check_mnk(m: int, n: int, k: int) -> None:
    if m < 0 or n < 0 or k < 0:
        raise ValueError("matrix dimensions must be non-negative")


def matmul(a: list[float], b: list[float], m: int, n: int, k: int,
           trans_a: bool = False, trans_b: bool = False) -> list[float]:
    """计算 op(A)(m×k) 与 op(B)(k×n) 的乘积，返回长度 m*n 的行主序 list。

    trans_a 为 True 时 A 以 k×m 存储；trans_b 为 True 时 B 以 n×k 存储。
    """
    _check_mnk(m, n, k)
    if len(a) < m * k:
        raise ValueError("a has fewer than m*k elements")
    if len(b) < k * n:
        raise ValueError("b has fewer than k*n elements")
    c = _zeros(m * n)
    matmul_into(c, a, b, m, n, k, trans_a, trans_b)
    return c


def matmul_into(c: list[float], a: list[float], b: list[float], m: int, n: int,
                k: int, trans_a: bool = False, trans_b: bool = False) -> None:
    """同 matmul，但把 m×n 结果写入已有的 c，避免额外分配。"""
    _check_mnk(m, n, k)
    if len(a) < m * k:
        raise ValueError("a has fewer than m*k elements")
    if len(b) < k * n:
        raise ValueError("b has fewer than k*n elements")
    if len(c) < m * n:
        raise ValueError("c has fewer than m*n elements")
    ta = cblas.TRANS if trans_a else cblas.NO_TRANS
    tb = cblas.TRANS if trans_b else cblas.NO_TRANS
    lda = m if trans_a else k
    ldb = k if trans_b else n
    cblas.dgemm(cblas.ROW_MAJOR, ta, tb, m, n, k,
                1.0, a, lda, b, ldb, 0.0, c, n)


def matvec(a: list[float], x: list[float], m: int, n: int,
           trans_a: bool = False) -> list[float]:
    """计算 op(A)·x，A 为 m×n 行主序；trans_a 时返回长度 n，否则 m。"""
    _check_mnk(m, n, 0)
    if len(a) < m * n:
        raise ValueError("a has fewer than m*n elements")
    in_len = m if trans_a else n
    out_len = n if trans_a else m
    if len(x) < in_len:
        raise ValueError("x is shorter than the input dimension")
    y = _zeros(out_len)
    ta = cblas.TRANS if trans_a else cblas.NO_TRANS
    cblas.dgemv(cblas.ROW_MAJOR, ta, m, n, 1.0, a, n, x, 1, 0.0, y, 1)
    return y


def dot(x: list[float], y: list[float]) -> float:
    """计算两个等长向量的点积。"""
    if len(x) != len(y):
        raise ValueError("vectors must have the same length")
    return cblas.ddot(len(x), x, 1, y, 1)
