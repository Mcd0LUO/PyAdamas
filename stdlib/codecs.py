"""codecs 的 PyAdamas 薄包装（纯 .py）。

只提供 encode(s, encoding="utf-8") / decode(b, encoding="utf-8")：
底层直接用 str.encode / bytes.decode。

与 CPython 的差异：
- 不做编码名规范化（CPython codecs.encode 支持别名查找如 "utf8"/"UTF-8"，
  底层 str.encode 也支持常见别名；生僻别名/自定义 codec 不支持）。
- 没有CodecInfo / register / lookup / open / IncrementalEncoder 等。
- bytes 只覆盖 ASCII 与 CPython bytes 共用的表示；非 ASCII 码点走 PA 的
  str <-> bytes 转换语义（见 docs/compatibility.md）。
"""

def encode(s: str, encoding: str = "utf-8") -> bytes:
    """等价于 CPython codecs.encode(s, encoding)。"""
    return s.encode(encoding)

def decode(b: bytes, encoding: str = "utf-8") -> str:
    """等价于 CPython codecs.decode(b, encoding)。"""
    return b.decode(encoding)
