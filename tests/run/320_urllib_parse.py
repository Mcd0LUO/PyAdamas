# urllib.parse 子集：拆分/拼接/相对合并/百分号编码/表单编码
from urllib.parse import (
    urlparse, urlsplit, urlunparse, urlunsplit, urljoin, urldefrag,
    quote, quote_plus, unquote, unquote_plus, quote_from_bytes,
    unquote_to_bytes, urlencode, parse_qs, parse_qsl,
)

u = urlparse("https://user:pw@example.com:8080/a/b;c?x=1&y=2#frag")
print(u.scheme, u.netloc, u.path)
print(u.params, u.query, u.fragment)
print(u)
print(len(u), u[0], u[2], u[-1], u.geturl())

s = urlsplit("http://example.com:80/p?q=1#f")
print(s)
print(s.scheme, s.netloc, s.path, s.query, s.fragment)
print(len(s), s[1], s[-1], s.geturl())

print(urlparse("//host/a;x?q#f"))
print(urlparse("mailto:someone@example.com").scheme,
      urlparse("mailto:someone@example.com").path)
print(urlparse("/local/path?q=1", "http"))
print(urlparse("example.com/x", "http").scheme)
print(urlsplit("http://a/b#c#d").fragment)

print(urlunparse(("http", "h", "/p", "params", "q=1", "frag")))
print(urlunparse(("http", "", "/p", "", "", "")))
print(urlunparse(("http", "", "", "", "", "")))
print(urlunsplit(("https", "h", "/p", "q", "f")))
print(urlunsplit(("http", "", "p", "", "")))

print(urljoin("http://a/b/c", "d"))
print(urljoin("http://a/b/", "d"))
print(urljoin("http://a/b/c/d;p=1", "../q;x"))
print(urljoin("http://a/b", "#f"))
print(urljoin("http://a/b/c", "//h/p"))
print(urljoin("a/b", "c"))
print(urljoin("http://a", ""))
print(urljoin("", "x/y"))
print(urljoin("http://a/b/c", "https://other/x"))

d = urldefrag("http://a/b#frag")
print(d.url, d.fragment)
print(d)
print(urldefrag("http://a/b"))

print(quote("a b/c?d=é&f"))
print(quote("a b/c", safe=""))
print(quote("é", safe=""))
print(quote_plus("a b&c=d"))
print(quote_plus("x/y", safe="/"))
print(unquote("%E4%B8%AD%E6%96%87"))
print(unquote("%zz%41"))
print(unquote_plus("a+b%20c"))
print(unquote_to_bytes("a%20b"))
print(quote_from_bytes(b"\x00\xffA b"))

print(urlencode({"a": "1", "b": "x y"}))
print(urlencode([("a", "1"), ("b", "2 3")]))
print(urlencode({"名字": "值"}))
print(parse_qs("a=1&a=2&b=x%20y"))
print(parse_qs("a=1&b=&c=3", True))
print(parse_qsl("a=1&a=2&b=x%20y"))
print(parse_qsl("a=1&b="))
print(parse_qsl("a=1;b=2", separator=";"))


# --- expect ---
# https user:pw@example.com:8080 /a/b
# c x=1&y=2 frag
# ParseResult(scheme='https', netloc='user:pw@example.com:8080', path='/a/b', params='c', query='x=1&y=2', fragment='frag')
# 6 https /a/b frag https://user:pw@example.com:8080/a/b;c?x=1&y=2#frag
# SplitResult(scheme='http', netloc='example.com:80', path='/p', query='q=1', fragment='f')
# http example.com:80 /p q=1 f
# 5 example.com:80 f http://example.com:80/p?q=1#f
# ParseResult(scheme='', netloc='host', path='/a', params='x', query='q', fragment='f')
# mailto someone@example.com
# ParseResult(scheme='http', netloc='', path='/local/path', params='', query='q=1', fragment='')
# http
# c#d
# http://h/p;params?q=1#frag
# http:///p
# http://
# https://h/p?q#f
# http:p
# http://a/b/d
# http://a/b/d
# http://a/b/q;x
# http://a/b#f
# http://h/p
# a/c
# http://a
# x/y
# https://other/x
# http://a/b frag
# DefragResult(url='http://a/b', fragment='frag')
# DefragResult(url='http://a/b', fragment='')
# a%20b/c%3Fd%3D%C3%A9%26f
# a%20b%2Fc
# %C3%A9
# a+b%26c%3Dd
# x/y
# 中文
# %zzA
# a b c
# b'a b'
# %00%FFA%20b
# a=1&b=x+y
# a=1&b=2+3
# %E5%90%8D%E5%AD%97=%E5%80%BC
# {'a': ['1', '2'], 'b': ['x y']}
# {'a': ['1'], 'b': [''], 'c': ['3']}
# [('a', '1'), ('a', '2'), ('b', 'x y')]
# [('a', '1')]
# [('a', '1'), ('b', '2')]
