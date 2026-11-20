"""urllib.parse —— URL 解析与百分号编码（纯 pyadamas 源码，stdlib3）。

覆盖日常面：urlparse / urlsplit / urlunparse / urlunsplit / urljoin /
urldefrag、quote / quote_plus / quote_from_bytes / unquote / unquote_plus /
unquote_to_bytes、urlencode / parse_qs / parse_qsl，以及 uses_* 表与
ParseResult / SplitResult / DefragResult 结果对象。

语义边界（轻量标尺，见 docs/stdlib-batch3.md）：
- 只收 str 输入：CPython 的 *Bytes 变体与 bytes/bytearray 输入不做；
- 结果对象是普通类（不是 tuple 子类）：字段读、下标读、len、repr、
  geturl 可用；元组解包与跨对象 == 比较不支持（拿字段自己比）；
- urlunparse / urlunsplit 按 CPython 文档口径收 5/6 元组；结果对象用
  .geturl()；
- query/fragment 的「空但存在」与「缺失」不做区分（CPython 用 None 区分，
  两种情况的拼接输出相同）；
- netloc 的 IDNA/NFKC 安全校验与 Windows 盘符特例不做；控制字符仍按
  WHATWG 规则从首尾/中部剔除。
"""

from typing import Optional

uses_relative: list[str] = [
    "", "ftp", "http", "gopher", "nntp", "imap", "wais", "file", "https",
    "shttp", "mms", "prospero", "rtsp", "rtsps", "rtspu", "sftp", "svn",
    "svn+ssh", "ws", "wss",
]

uses_netloc: list[str] = [
    "", "ftp", "http", "gopher", "nntp", "telnet", "imap", "wais", "file",
    "mms", "https", "shttp", "snews", "prospero", "rtsp", "rtsps", "rtspu",
    "rsync", "svn", "svn+ssh", "sftp", "nfs", "git", "git+ssh", "ws", "wss",
]

uses_params: list[str] = [
    "", "ftp", "hdl", "prospero", "http", "imap", "https", "shttp", "mms",
    "rtsp", "rtsps", "rtspu", "sip", "sips", "tel", "ws", "wss",
]

_SCHEME_CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+.-"
_ALWAYS_SAFE = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.-~"
_HEXDIG = "0123456789ABCDEFabcdef"
_HEX = "0123456789ABCDEF"


# ── 结果对象 ────────────────────────────────────────────────────


class _Parts:
    """内部结果：记录 netloc 是否真的出现过（urljoin 要靠它决定 //）。"""

    scheme: str
    netloc: str
    has_netloc: bool
    path: str
    query: str
    fragment: str

    def __init__(self, scheme: str, netloc: str, has_netloc: bool, path: str,
                 query: str, fragment: str) -> None:
        self.scheme = scheme
        self.netloc = netloc
        self.has_netloc = has_netloc
        self.path = path
        self.query = query
        self.fragment = fragment


class ParseResult:
    """urlparse 的结果：scheme/netloc/path/params/query/fragment。"""

    scheme: str
    netloc: str
    path: str
    params: str
    query: str
    fragment: str

    def __init__(self, scheme: str, netloc: str, path: str, params: str,
                 query: str, fragment: str) -> None:
        self.scheme = scheme
        self.netloc = netloc
        self.path = path
        self.params = params
        self.query = query
        self.fragment = fragment

    def geturl(self) -> str:
        return urlunparse((self.scheme, self.netloc, self.path, self.params,
                           self.query, self.fragment))

    def __len__(self) -> int:
        return 6

    def __getitem__(self, i: int) -> str:
        if i < 0:
            i = i + 6
        if i == 0:
            return self.scheme
        if i == 1:
            return self.netloc
        if i == 2:
            return self.path
        if i == 3:
            return self.params
        if i == 4:
            return self.query
        if i == 5:
            return self.fragment
        raise IndexError("tuple index out of range")

    def __repr__(self) -> str:
        return ("ParseResult(scheme=" + repr(self.scheme) + ", netloc=" +
                repr(self.netloc) + ", path=" + repr(self.path) + ", params=" +
                repr(self.params) + ", query=" + repr(self.query) +
                ", fragment=" + repr(self.fragment) + ")")


class SplitResult:
    """urlsplit 的结果：scheme/netloc/path/query/fragment。"""

    scheme: str
    netloc: str
    path: str
    query: str
    fragment: str

    def __init__(self, scheme: str, netloc: str, path: str, query: str,
                 fragment: str) -> None:
        self.scheme = scheme
        self.netloc = netloc
        self.path = path
        self.query = query
        self.fragment = fragment

    def geturl(self) -> str:
        return urlunsplit((self.scheme, self.netloc, self.path, self.query,
                           self.fragment))

    def __len__(self) -> int:
        return 5

    def __getitem__(self, i: int) -> str:
        if i < 0:
            i = i + 5
        if i == 0:
            return self.scheme
        if i == 1:
            return self.netloc
        if i == 2:
            return self.path
        if i == 3:
            return self.query
        if i == 4:
            return self.fragment
        raise IndexError("tuple index out of range")

    def __repr__(self) -> str:
        return ("SplitResult(scheme=" + repr(self.scheme) + ", netloc=" +
                repr(self.netloc) + ", path=" + repr(self.path) + ", query=" +
                repr(self.query) + ", fragment=" + repr(self.fragment) + ")")


class DefragResult:
    """urldefrag 的结果：url/fragment。"""

    url: str
    fragment: str

    def __init__(self, url: str, fragment: str) -> None:
        self.url = url
        self.fragment = fragment

    def geturl(self) -> str:
        return self.url

    def __len__(self) -> int:
        return 2

    def __getitem__(self, i: int) -> str:
        if i < 0:
            i = i + 2
        if i == 0:
            return self.url
        if i == 1:
            return self.fragment
        raise IndexError("tuple index out of range")

    def __repr__(self) -> str:
        return ("DefragResult(url=" + repr(self.url) + ", fragment=" +
                repr(self.fragment) + ")")


# ── 解析 ────────────────────────────────────────────────────────


def _strip_c0(text: str) -> str:
    """去掉前导 C0 控制字符与空格（WHATWG basic URL parser 口径）。"""
    i = 0
    n = len(text)
    while i < n:
        if ord(text[i]) <= 32:
            i += 1
        else:
            break
    return text[i:]


def _clean(text: str) -> str:
    for ch in ("\t", "\r", "\n"):
        text = text.replace(ch, "")
    return text


def _splitnetloc(url: str, start: int) -> tuple[str, str]:
    delim = len(url)
    for ch in "/?#":
        w = url.find(ch, start)
        if w >= 0 and w < delim:
            delim = w
    return url[start:delim], url[delim:]


def _splitparams(url: str) -> tuple[str, str]:
    if "/" in url:
        i = url.find(";", url.rfind("/"))
        if i < 0:
            return url, ""
    else:
        i = url.find(";")
    if i < 0:
        return url, ""
    return url[:i], url[i + 1:]


def _is_scheme(cand: str) -> bool:
    if not cand:
        return False
    if not (ord(cand[0]) < 128 and cand[0].isalpha()):
        return False
    for ch in cand:
        if ch not in _SCHEME_CHARS:
            return False
    return True


def _rawsplit(url: str, scheme: str = "", allow_fragments: bool = True) -> _Parts:
    url = _clean(_strip_c0(url))
    scheme = _clean(_strip_c0(scheme))
    i = url.find(":")
    if i > 0 and _is_scheme(url[:i]):
        scheme = url[:i].lower()
        url = url[i + 1:]
    netloc = ""
    has_netloc = False
    if url[:2] == "//":
        netloc, url = _splitnetloc(url, 2)
        has_netloc = True
        if ("[" in netloc) != ("]" in netloc):
            raise ValueError("Invalid IPv6 URL")
    fragment = ""
    if allow_fragments:
        i = url.find("#")
        if i >= 0:
            fragment = url[i + 1:]
            url = url[:i]
    query = ""
    i = url.find("?")
    if i >= 0:
        query = url[i + 1:]
        url = url[:i]
    return _Parts(scheme, netloc, has_netloc, url, query, fragment)


def urlsplit(url: str, scheme: str = "", allow_fragments: bool = True) -> SplitResult:
    """按 5 段拆分 URL：scheme://netloc/path?query#fragment。"""
    p = _rawsplit(url, scheme, allow_fragments)
    return SplitResult(p.scheme, p.netloc, p.path, p.query, p.fragment)


def urlparse(url: str, scheme: str = "", allow_fragments: bool = True) -> ParseResult:
    """按 6 段拆分 URL（比 urlsplit 多出 path 末尾的 ;params）。"""
    p = _rawsplit(url, scheme, allow_fragments)
    path = p.path
    params = ""
    if p.scheme in uses_params and ";" in path:
        path, params = _splitparams(path)
    return ParseResult(p.scheme, p.netloc, path, params, p.query, p.fragment)


# ── 拼接 ────────────────────────────────────────────────────────


def _urlunsplit(scheme: str, netloc: str, has_netloc: bool, url: str,
                query: str, fragment: str) -> str:
    if has_netloc:
        if url:
            if url[0] != "/":
                url = "/" + url
        url = "//" + netloc + url
    else:
        if url[:2] == "//":
            url = "//" + url
    if scheme:
        url = scheme + ":" + url
    if query:
        url = url + "?" + query
    if fragment:
        url = url + "#" + fragment
    return url


def _unparse_netloc(scheme: str, netloc: str, url: str) -> bool:
    """urlunparse 口径：空 netloc 时是否还留 //。"""
    if netloc:
        return True
    if not scheme:
        return False
    if scheme not in uses_netloc:
        return False
    if not url:
        return True
    return url[0] == "/"


def urlunparse(components: tuple[str, str, str, str, str, str]) -> str:
    """把 urlparse 的 6 段元组拼回 URL。"""
    scheme, netloc, url, params, query, fragment = components
    if params:
        url = url + ";" + params
    return _urlunsplit(scheme, netloc, _unparse_netloc(scheme, netloc, url),
                       url, query, fragment)


def urlunsplit(components: tuple[str, str, str, str, str]) -> str:
    """把 urlsplit 的 5 段元组拼回 URL。"""
    scheme, netloc, url, query, fragment = components
    return _urlunsplit(scheme, netloc, _unparse_netloc(scheme, netloc, url),
                       url, query, fragment)


def urljoin(base: str, url: str, allow_fragments: bool = True) -> str:
    """把可能相对的 url 相对 base 解析成绝对 URL（RFC 3986 合并）。"""
    if not base:
        return url
    if not url:
        return base
    b = _rawsplit(base, "", allow_fragments)
    u = _rawsplit(url, "", allow_fragments)
    scheme = u.scheme
    if not scheme:
        scheme = b.scheme
    if scheme != b.scheme or (scheme and scheme not in uses_relative):
        return url
    netloc = u.netloc
    has_netloc = u.has_netloc
    if not scheme or scheme in uses_netloc:
        if has_netloc:
            return _urlunsplit(scheme, netloc, True, u.path, u.query, u.fragment)
        netloc = b.netloc
        has_netloc = b.has_netloc
    if not u.path:
        path = b.path
        query = u.query
        fragment = u.fragment
        if not query:
            query = b.query
            if not fragment:
                fragment = b.fragment
        if not has_netloc and path[:2] == "//":
            has_netloc = True
        return _urlunsplit(scheme, netloc, has_netloc, path, query, fragment)
    base_parts = b.path.split("/")
    if base_parts[len(base_parts) - 1] != "":
        base_parts.pop()
    if u.path[:1] == "/":
        segments = u.path.split("/")
    else:
        segments: list[str] = []
        for seg in base_parts:
            segments.append(seg)
        for seg in u.path.split("/"):
            segments.append(seg)
        if len(segments) > 2:
            keep: list[str] = []
            i = 0
            while i < len(segments):
                if i == 0 or i == len(segments) - 1:
                    keep.append(segments[i])
                elif segments[i] != "":
                    keep.append(segments[i])
                i += 1
            segments = keep
    resolved: list[str] = []
    for seg in segments:
        if seg == "..":
            if len(resolved) > 0:
                resolved.pop()
        elif seg == ".":
            continue
        else:
            resolved.append(seg)
    last = segments[len(segments) - 1]
    if last == "." or last == "..":
        resolved.append("")
    joined = "/".join(resolved)
    if not joined:
        joined = "/"
    return _urlunsplit(scheme, netloc, has_netloc, joined, u.query, u.fragment)


def urldefrag(url: str) -> DefragResult:
    """拆出 #fragment，返回 (去掉 fragment 的 url, fragment)。"""
    if "#" in url:
        p = _rawsplit(url, "", True)
        return DefragResult(_urlunsplit(p.scheme, p.netloc, p.has_netloc,
                                        p.path, p.query, ""), p.fragment)
    return DefragResult(url, "")


# ── 百分号编码 ──────────────────────────────────────────────────


def _quote_text(string: str, safe: str, encoding: str, errors: str) -> str:
    out: list[str] = []
    for ch in string:
        if ch in _ALWAYS_SAFE or ch in safe:
            out.append(ch)
        else:
            for b in ch.encode(encoding, errors):
                out.append("%" + _HEX[b >> 4] + _HEX[b & 15])
    return "".join(out)


def quote(string: str, safe: str = "/", encoding: str = "utf-8",
          errors: str = "replace") -> str:
    """把 str 编码成 %XX 转义（保留字母数字与 _.-~ 和 safe）。"""
    return _quote_text(string, safe, encoding, errors)


def quote_plus(string: str, safe: str = "", encoding: str = "utf-8",
               errors: str = "replace") -> str:
    """同 quote，但空格变成 +（表单编码）。"""
    return _quote_text(string, safe + " ", encoding, errors).replace(" ", "+")


def quote_from_bytes(data: bytes, safe: str = "/") -> str:
    """把 bytes 编码成 %XX 转义。"""
    out: list[str] = []
    for b in data:
        ch = bytes([b]).decode("latin-1")
        if ch in _ALWAYS_SAFE or ch in safe:
            out.append(ch)
        else:
            out.append("%" + _HEX[b >> 4] + _HEX[b & 15])
    return "".join(out)


def _is_hex(ch: str) -> bool:
    return ch in _HEXDIG


def unquote_to_bytes(string: str) -> bytes:
    """解码 %XX 转义，得到原始字节（不按字符集解码）。"""
    out: list[int] = []
    i = 0
    n = len(string)
    while i < n:
        ch = string[i]
        if ch == "%" and i + 2 < n and _is_hex(string[i + 1]) and _is_hex(string[i + 2]):
            out.append(int(string[i + 1:i + 3], 16))
            i += 3
        else:
            for b in ch.encode("utf-8"):
                out.append(b)
            i += 1
    return bytes(out)


def unquote(string: str, encoding: str = "utf-8", errors: str = "replace") -> str:
    """解码 %XX 转义并按 encoding 解码成 str。"""
    return unquote_to_bytes(string).decode(encoding, errors)


def unquote_plus(string: str, encoding: str = "utf-8",
                 errors: str = "replace") -> str:
    """同 unquote，但先把 + 变回空格。"""
    return unquote(string.replace("+", " "), encoding, errors)


# ── 表单编码 ────────────────────────────────────────────────────


def _encode_pair(k: str, v: str, safe: str, encoding: str, errors: str) -> str:
    kk = _quote_text(k, safe + " ", encoding, errors).replace(" ", "+")
    vv = _quote_text(v, safe + " ", encoding, errors).replace(" ", "+")
    return kk + "=" + vv


def _encode_dict(d: dict[str, unknown], safe: str, encoding: str, errors: str) -> str:
    out: list[str] = []
    for k in d:
        out.append(_encode_pair(str(k), str(d[k]), safe, encoding, errors))
    return "&".join(out)


def _encode_pairs(pairs: list[unknown], safe: str, encoding: str, errors: str) -> str:
    out: list[str] = []
    for p in pairs:
        out.append(_encode_pair(str(p[0]), str(p[1]), safe, encoding, errors))
    return "&".join(out)


def urlencode(query: unknown, doseq: bool = False, safe: str = "",
              encoding: str = "utf-8", errors: str = "replace") -> str:
    """把 dict 或 (key, value) 序列编码成 a=1&b=2。

    doseq 只做接口兼容：值里的 list 退化成 str(value)（不做逐项展开）。
    """
    if isinstance(query, dict):
        return _encode_dict(query, safe, encoding, errors)
    return _encode_pairs(query, safe, encoding, errors)


def parse_qsl(qs: str, keep_blank_values: bool = False,
              strict_parsing: bool = False, encoding: str = "utf-8",
              errors: str = "replace", max_num_fields: int = -1,
              separator: str = "&") -> list[tuple[str, str]]:
    """解析 a=1&b=2 成 (name, value) 列表。"""
    out: list[tuple[str, str]] = []
    if not qs:
        return out
    fields = qs.split(separator)
    for field in fields:
        if not field:
            continue
        if "=" in field:
            name, value = field.split("=", 1)
        else:
            name = field
            value = ""
        name = unquote_plus(name, encoding, errors)
        value = unquote_plus(value, encoding, errors)
        if strict_parsing and "=" not in field:
            raise ValueError("bad query field: " + repr(field))
        if keep_blank_values or value != "":
            out.append((name, value))
        if max_num_fields >= 0 and len(out) > max_num_fields:
            raise ValueError("Max number of fields exceeded")
    return out


def parse_qs(qs: str, keep_blank_values: bool = False,
             strict_parsing: bool = False, encoding: str = "utf-8",
             errors: str = "replace", max_num_fields: int = -1,
             separator: str = "&") -> dict[str, list[str]]:
    """解析 a=1&a=2 成 {a: [1, 2]}。"""
    out: dict[str, list[str]] = {}
    for name, value in parse_qsl(qs, keep_blank_values, strict_parsing,
                                 encoding, errors, max_num_fields, separator):
        if name in out:
            out[name].append(value)
        else:
            out[name] = [value]
    return out
