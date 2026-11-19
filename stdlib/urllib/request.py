"""urllib.request —— libcurl 原生 HTTP 客户端（stdlib2）。

高层 API 按 CPython urllib.request 的日常面：urlopen(url, data=None,
timeout=..., headers=..., method=...) 返回 Response（.status/.code/
.read()/.headers/.geturl()/.getcode()/.info()/上下文管理）。
GET/POST/自定义头/超时/重定向/HTTPS（系统 libcurl 自带 TLS，默认验证
证书）。热路径走 cimport _curlc（link curl + curl_glue.c）：CURL 句柄、
回调、缓冲增长全在 C 侧，Python 只传字符串与出参数组。

语义边界（轻量标尺，见 docs/stdlib-batch2.md）：
- 整读整写：响应体一次收进内存（同 GzipFile 口径），超大响应用桥接。
- urlopen 签名是子集：headers/method 是直接关键字（CPython 要经
  Request 对象）；Request/OpenerDirector/handler 体系不做。
- data 收 bytes（str 按 utf-8 直通，CPython 要求 bytes）；有 data 默认
  POST 并补 Content-Type: application/x-www-form-urlencoded（同 CPython）。
- timeout 是整请求超时（CURLOPT_TIMEOUT_MS），CPython 是逐 socket 操作；
  省略/负值＝不超时。超时抛 URLError（reason="timed out"）。
- 重定向跟随（MAXREDIRS=10）；301/302/303 的 POST→GET 与 307/308 保持
  方法按 libcurl 默认口径。
- 默认 User-Agent 是 libcurl/x.y（CPython 是 Python-urllib/3.x）；
  可用 headers 覆盖。Accept-Encoding 不自动压缩（同 CPython identity）。
- HTTP >= 400 抛 HTTPError（URLError 子类，带 .code/.status/.headers/
  .read() 响应面）；网络层失败抛 URLError（OSError 子类，.reason）。
- 只支持 http/https；其它 scheme 抛 URLError（unknown url type）。
- urlretrieve/Request/urllib.parse 等长尾：from python import 桥接。
"""

cimport _curlc as _c
from urllib.error import URLError, HTTPError, HTTPMessage
from typing import cast


_ERRCAP: int = 256
_EFFCAP: int = 2048


def _trim_decode(buf: bytes, n: int) -> str:
    return buf[:n].decode("utf-8", "replace")


def _perform(url: str, method: str, body: bytes, blen: int, hstr: str, ms: int,
             out: list[int], err: bytes, eff: bytes) -> int:
    # 独立小帧 + 全类型化形参：复杂帧里直接发 12 参 cimport 调用会撞基线
    # 的 ptr 装填 bug（unknown 重赋值的 bytes 局部传成对象+8；见
    # docs/stdlib-batch2.md 已知问题，curl_probe2.py 最小复现）。
    return _c.request(url, method, body, blen, hstr, ms, 1, out, err, _ERRCAP, eff, _EFFCAP)


def _reason_of(raw: str) -> str:
    blocks = raw.split("\r\n\r\n")
    last = ""
    for b in blocks:
        if len(b.strip()) > 0:
            last = b
    lines = last.split("\r\n")
    if len(lines) == 0:
        return ""
    parts = lines[0].split(" ", 2)
    if len(parts) >= 3:
        return parts[2]
    return ""


class Response:
    """urlopen 的响应对象（整读口径，见模块文档）。"""
    status: int
    code: int
    url: str
    headers: HTTPMessage
    closed: bool
    _buf: bytes
    _pos: int

    def __init__(self, url: str, status: int, headers: HTTPMessage, body: bytes) -> None:
        self.url = url
        self.status = status
        self.code = status
        self.headers = headers
        self.closed = False
        self._buf = body
        self._pos = 0

    def read(self, size: int = -1) -> bytes:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        n = len(self._buf)
        end = n
        if size >= 0 and self._pos + size < n:
            end = self._pos + size
        chunk = self._buf[self._pos:end]
        self._pos = end
        return chunk

    def readline(self, size: int = -1) -> bytes:
        if self.closed:
            raise ValueError("I/O operation on closed file")
        n = len(self._buf)
        if self._pos >= n:
            return b""
        nl = self._buf.find(b"\n", self._pos)
        end = n
        if nl >= 0:
            end = nl + 1
        if size >= 0 and self._pos + size < end:
            end = self._pos + size
        chunk = self._buf[self._pos:end]
        self._pos = end
        return chunk

    def readlines(self) -> list[bytes]:
        out: list[bytes] = []
        while True:
            line = self.readline()
            if len(line) == 0:
                break
            out.append(line)
        return out

    def close(self) -> None:
        self.closed = True

    def geturl(self) -> str:
        return self.url

    def getcode(self) -> int:
        return self.status

    def info(self) -> HTTPMessage:
        return self.headers

    def __iter__(self) -> list[bytes]:
        return self.readlines()

    def __enter__(self) -> "Response":
        return self

    def __exit__(self, exc_type: object, exc_val: object, exc_tb: object) -> bool:
        self.close()
        return False


class Request:
    """请求对象（CPython urllib.request.Request 的日常面）。

    url/data/headers/method；headers 收 dict[str,str]。get_method()
    按 method > data(POST) > GET 推导。add_header/unredirected 等长尾不做。
    """
    full_url: str
    data: object
    headers: dict[str, str]
    method: object

    def __init__(self, url: str, data: object = None, headers: object = None, *,
                 method: object = None) -> None:
        self.full_url = url
        self.data = data
        self.headers = {}
        if not (headers is None):
            self.headers = headers
        self.method = method

    def get_method(self) -> object:
        if not (self.method is None):
            return self.method
        if not (self.data is None):
            return "POST"
        return "GET"


def urlopen(url: object, data: object = None, timeout: float = -1.0, *,
            headers: object = None, method: object = None) -> Response:
    u: str = ""
    req_data: object = data
    req_headers: object = headers
    req_method: object = method
    if isinstance(url, Request):
        rq: Request = url
        u = rq.full_url
        if data is None:
            req_data = rq.data
        if headers is None:
            req_headers = rq.headers
        if method is None:
            req_method = rq.method
    else:
        u = url
    pos = u.find("://")
    if pos <= 0:
        raise ValueError("unknown url type: '" + u + "'")
    scheme = u[:pos].lower()
    if scheme != "http" and scheme != "https":
        raise URLError("unknown url type: " + scheme)
    m = ""
    if not (req_method is None):
        m = req_method
    body = b""
    blen = -1
    if not (req_data is None):
        body = req_data
        blen = len_bytes(body)
        if len(m) == 0:
            m = "POST"
    hd: dict[str, str] = {}
    if not (req_headers is None):
        hd = req_headers
    lines: list[str] = []
    has_ct = False
    for k in hd:
        lines.append(k + ": " + hd[k])
        if k.lower() == "content-type":
            has_ct = True
    if blen >= 0 and not has_ct:
        lines.append("Content-Type: application/x-www-form-urlencoded")
    hstr = "\n".join(lines)
    ms = 0
    if timeout > 0:
        ms = int(timeout * 1000)
    out: list[int] = [0, 0, 0, 0, 0, 0, 0, 0, 0]
    err = bytes(_ERRCAP)
    eff = bytes(_EFFCAP)
    rc = _perform(u, m, body, blen, hstr, ms, out, err, eff)
    if rc != 0:
        if rc == _c.CURLE_OPERATION_TIMEDOUT:
            # 连接已建立后的超时＝传输阶段（CPython 抛裸 TimeoutError）；
            # 连接阶段超时 CPython 包成 URLError。按 connect time 区分。
            if out[8] > 0:
                raise TimeoutError("timed out")
            raise URLError("timed out")
        if rc == _c.CURLE_TOO_MANY_REDIRECTS:
            raise URLError("too many redirects")
        raise URLError(_trim_decode(err, out[6]))
    n = out[2]
    buf = bytes(n)
    _c.copy(buf, out[3], n)
    _c.free(out[3])
    hn = out[4]
    hbuf = bytes(hn)
    _c.copy(hbuf, out[5], hn)
    _c.free(out[5])
    hraw = hbuf.decode("utf-8", "replace")
    msg = HTTPMessage(hraw)
    status = out[1]
    final_url = _trim_decode(eff, out[7])
    if status >= 400:
        raise HTTPError(final_url, status, _reason_of(hraw), msg, buf)
    return Response(final_url, status, msg, buf)
