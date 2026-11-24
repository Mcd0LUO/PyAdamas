# urllib.parse 百分号编码边界（CPython 实跑取期望）。
from urllib.parse import (quote, quote_plus, unquote, unquote_plus,
                          quote_from_bytes, unquote_to_bytes)

cases = ["a b", "a/b", "a%20b", "100%", "%zz", "%4", "\u00e9", "\u4e2d\u6587",
         "~_.-", "a+b", "", " ", "\\", "\x01\x1f\x7f"]
for s in cases:
    print(quote(s), quote(s, safe=""), quote_plus(s))
for s in cases:
    print(unquote(s) == s, unquote_plus(s) == s, len(unquote(s)))
print(quote_from_bytes(b"\x00\x80\xff abc/"))
print(quote_from_bytes(b"a b", safe=" "))
print(unquote_to_bytes("%41%4a%4A").decode("latin-1"))
print(unquote_to_bytes("%4").decode("latin-1"), len(unquote_to_bytes("%")))
print(unquote("a%2"), len(unquote("a%")))

# --- expect ---
# a%20b a%20b a+b
# a/b a%2Fb a%2Fb
# a%2520b a%2520b a%2520b
# 100%25 100%25 100%25
# %25zz %25zz %25zz
# %254 %254 %254
# %C3%A9 %C3%A9 %C3%A9
# %E4%B8%AD%E6%96%87 %E4%B8%AD%E6%96%87 %E4%B8%AD%E6%96%87
# ~_.- ~_.- ~_.-
# a%2Bb a%2Bb a%2Bb
#   
# %20 %20 +
# %5C %5C %5C
# %01%1F%7F %01%1F%7F %01%1F%7F
# True True 3
# True True 3
# False False 3
# True True 4
# True True 3
# True True 2
# True True 1
# True True 2
# True True 4
# True False 3
# True True 0
# True True 1
# True True 1
# True True 3
# %00%80%FF%20abc/
# a b
# AJJ
# %4 1
# a%2 2
