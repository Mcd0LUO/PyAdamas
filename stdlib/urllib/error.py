"""urllib.error —— URLError / HTTPError（stdlib2，libcurl 口径）。

错误类型与 CPython 对齐：URLError 是 OSError 子类（网络层失败），
HTTPError 是 URLError 子类（HTTP >= 400，同时是响应对象：可读 body/
headers/status）。消息合理即可，不逐字对齐 CPython。

HTTPMessage（响应头集合）按 CPython 的放置口径在 urllib.request 侧
定义；这里为避免循环导入放在本模块，urllib.request 再导出。
"""

from typing import cast


class URLError(OSError):
    reason: object

    def __init__(self, reason: object) -> None:
        self.reason = reason

    def __str__(self) -> str:
        return "<urlopen error " + str(self.reason) + ">"


class HTTPMessage:
    """响应头（大小写不敏感 get；保留重复头与原始文本）。"""
    _pairs: list[tuple[str, str]]
    _raw: str

    def __init__(self, raw: str) -> None:
        self._raw = raw
        self._pairs = []
        blocks = raw.split("\r\n\r\n")
        last = ""
        for b in blocks:
            if len(b.strip()) > 0:
                last = b
        lines = last.split("\r\n")
        i = 1
        while i < len(lines):
            ln = lines[i]
            pos = ln.find(":")
            if pos > 0:
                self._pairs.append((ln[:pos], ln[pos + 1:].strip()))
            i += 1

    def get(self, name: str, default: object = None) -> object:
        low = name.lower()
        for p in self._pairs:
            if p[0].lower() == low:
                return p[1]
        return default

    def get_all(self, name: str) -> list[str]:
        out: list[str] = []
        low = name.lower()
        for p in self._pairs:
            if p[0].lower() == low:
                out.append(p[1])
        return out

    def items(self) -> list[tuple[str, str]]:
        return list(self._pairs)

    def keys(self) -> list[str]:
        out: list[str] = []
        for p in self._pairs:
            out.append(p[0])
        return out

    def __contains__(self, name: object) -> bool:
        s: str = name
        low = s.lower()
        for p in self._pairs:
            if p[0].lower() == low:
                return True
        return False

    def __getitem__(self, name: str) -> str:
        low = name.lower()
        for p in self._pairs:
            if p[0].lower() == low:
                return p[1]
        raise KeyError(name)

    def __str__(self) -> str:
        return self._raw


class HTTPError(URLError):
    url: str
    code: int
    status: int
    msg: str
    headers: HTTPMessage
    _body: bytes
    _pos: int

    def __init__(self, url: str, code: int, msg: str, headers: HTTPMessage,
                 body: bytes) -> None:
        self.url = url
        self.code = code
        self.status = code
        self.msg = msg
        self.headers = headers
        self._body = body
        self._pos = 0
        self.reason = msg

    def __str__(self) -> str:
        return "HTTP Error " + str(self.code) + ": " + self.msg

    def read(self, size: int = -1) -> bytes:
        n = len(self._body)
        end = n
        if size >= 0 and self._pos + size < n:
            end = self._pos + size
        chunk = self._body[self._pos:end]
        self._pos = end
        return chunk

    def geturl(self) -> str:
        return self.url

    def getcode(self) -> int:
        return self.code

    def info(self) -> HTTPMessage:
        return self.headers

    def close(self) -> None:
        pass
