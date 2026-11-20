"""secrets —— 生成安全的随机数/令牌（纯 pyadamas 源码，stdlib3）。

覆盖 token_bytes / token_hex / token_urlsafe / randbelow / randbits /
choice / compare_digest（以及别名 token_urlsafe 用到的 base64 路径）。

语义边界（轻量标尺）：
- **随机源是内建 random 模块**：pyadamas 运行时没有 os.urandom/getrandom
  原语，纯 PyAdamas 源码拿不到内核熵。因此这里生成的令牌是「伪随机、
  不可预测性弱于 CPython」，**不要用于真实的安全用途**；接口与计算口径
  （字节数、编码、区间、拒绝采样）与 CPython 一致。
- nbytes 省略/负数走 CPython 的默认 32 字节（CPython 用 None 哨兵，这里用
  -1，因为 pa 的 int 形参默认值只能是字面量）。
- randbits(k) 走 int64：k <= 63 结果为正；k > 63 按 int64 回绕（文档化）。
"""

import base64
import random

_HEX = "0123456789abcdef"

_DEFAULT_BYTES = 32


def token_bytes(nbytes: int = -1) -> bytes:
    """返回 nbytes 个随机字节（默认 32）。"""
    n = nbytes
    if n < 0:
        n = _DEFAULT_BYTES
    out: list[int] = []
    i = 0
    while i < n:
        out.append(random.getrandbits(8))
        i += 1
    return bytes(out)


def token_hex(nbytes: int = -1) -> str:
    """返回 2*nbytes 个十六进制字符（默认 64）。"""
    data = token_bytes(nbytes)
    out: list[str] = []
    for b in data:
        out.append(_HEX[b >> 4])
        out.append(_HEX[b & 15])
    return "".join(out)


def token_urlsafe(nbytes: int = -1) -> str:
    """返回 URL 安全的 base64 文本（去掉 = 填充）。"""
    data = token_bytes(nbytes)
    text = base64.urlsafe_b64encode(data).decode("ascii")
    n = len(text)
    while n > 0:
        if text[n - 1] == "=":
            n -= 1
        else:
            break
    return text[:n]


def randbits(k: int) -> int:
    """返回 k 个随机比特组成的 int（k <= 63 时非负）。"""
    if k < 0:
        raise ValueError("number of bits must be non-negative")
    if k == 0:
        return 0
    value = 0
    remaining = k
    while remaining > 0:
        take = 8
        if remaining < 8:
            take = remaining
        value = value * 256 + random.getrandbits(take)
        remaining -= take
    return value


def randbelow(n: int) -> int:
    """返回 [0, n) 的随机 int（拒绝采样，无模偏差）。"""
    if n <= 0:
        raise ValueError("Upper bound must be positive.")
    k = n.bit_length()
    r = randbits(k)
    while r >= n:
        r = randbits(k)
    return r


def _choice_list(seq: list[unknown]) -> unknown:
    n = len(seq)
    if n == 0:
        raise IndexError("Cannot choose from an empty sequence")
    return seq[randbelow(n)]


def _choice_str(seq: str) -> str:
    n = len(seq)
    if n == 0:
        raise IndexError("Cannot choose from an empty sequence")
    return seq[randbelow(n)]


def choice(seq: unknown) -> unknown:
    """从非空序列（list/tuple/str）里等概率取一个元素。"""
    if isinstance(seq, str):
        return _choice_str(seq)
    return _choice_list(seq)


def _str_bytes(s: str) -> bytes:
    return s.encode()


def _bytes_id(b: bytes) -> bytes:
    return b


def _digest_bytes(x: unknown, what: str) -> bytes:
    if isinstance(x, bytes):
        return _bytes_id(x)
    if isinstance(x, str):
        return _str_bytes(x)
    raise TypeError(what + " must be bytes or str")


def compare_digest(a: unknown, b: unknown) -> bool:
    """恒定时间比较两个 str/bytes（长度不同直接 False）。

    pa 的 bytes 与 str 共用运行期表示，unknown 实参上传进来分不出类型，
    所以 CPython 的「str/bytes 混比抛 TypeError」不复刻；同类型输入
    （含任意字节值的 bytes）比较结果一致。
    """
    left = _digest_bytes(a, "a")
    right = _digest_bytes(b, "b")
    if len(left) != len(right):
        return False
    diff = 0
    i = 0
    while i < len(left):
        diff = diff | (left[i] ^ right[i])
        i += 1
    return diff == 0
