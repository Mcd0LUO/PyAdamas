"""uuid 模块的 pyadamas 子集（纯 pyadamas 源码，随编译器分发）。

覆盖 UUID / uuid4 / NAMESPACE_* / getnode 的常用行为：
- uuid4() 用 random.getrandbits(128) 生成 RFC 4122 version 4 的随机 UUID；
- UUID("xxxx-...") 接受带连字符或纯 hex 的 32 位 hex 字符串；
- str(u) 输出标准 8-4-4-4-12 形式，u.hex 是 32 位小写 hex。

语义边界：没有 uuid1/uuid3/uuid5（缺时钟序列/节点与 bytes 原语，
见 docs/stdlib-gaps.md）；不做 RFC 版本位校验。
"""

import random

NAMESPACE_DNS = "6ba7b810-9dad-11d1-80b4-00c04fd430c8"
NAMESPACE_URL = "6ba7b811-9dad-11d1-80b4-00c04fd430c8"
NAMESPACE_OID = "6ba7b812-9dad-11d1-80b4-00c04fd430c8"
NAMESPACE_X500 = "6ba7b814-9dad-11d1-80b4-00c04fd430c8"


class UUID:
    hex: str

    def __init__(self, hexstr: str) -> None:
        h = hexstr.replace("-", "")
        if len(h) != 32:
            raise ValueError("badly formed hexadecimal UUID string")
        self.hex = h

    def __str__(self) -> str:
        h = self.hex
        return (h[0:8] + "-" + h[8:12] + "-" + h[12:16] + "-" + h[16:20]
                + "-" + h[20:32])

    def __repr__(self) -> str:
        return "UUID('" + str(self) + "')"


def uuid4() -> UUID:
    return UUID("%032x" % random.getrandbits(128))


def getnode() -> int:
    """没有网卡地址可读，返回固定值（文档已声明不是真实 MAC）。"""
    return 0
