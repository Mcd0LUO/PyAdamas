"""urllib.parse 基准：拆 URL + quote/unquote/parse_qs。"""
from urllib.parse import urlsplit, urljoin, quote, unquote, parse_qs

urls = ["https://user:pw@example.com:8080/a/b;c?x=1&y=2#frag",
        "http://example.com/path/to/thing?q=a%20b&n=3",
        "//host/a/b", "mailto:someone@example.com"]

total = 0
r = 0
while r < 8000:
    for u in urls:
        p = urlsplit(u)
        total += len(p.path) + len(p.netloc)
    total += len(quote("a b/c?d=e&f g", safe="/"))
    total += len(unquote("a%20b%2Fc%41"))
    total += len(urljoin("http://a/b/c/d;p=1", "../q;x"))
    total += len(parse_qs("a=1&b=2&a=3"))
    r += 1
print(total)
