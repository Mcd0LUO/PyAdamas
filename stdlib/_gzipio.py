"""_gzipio —— gzip 模块的内部二进制 IO 助手。

单独成模块是因为 gzip.py 自己定义了 open（会遮蔽内置 open），
文件读写必须放在没有遮蔽的模块里。
"""

from typing import cast


def read_bin(path: str) -> bytes:
    f = open(path, "rb")
    data: bytes = cast(bytes, f.read())
    f.close()
    return data


def write_bin(path: str, data: bytes) -> None:
    f = open(path, "wb")
    f.write(cast(str, data))
    f.close()
