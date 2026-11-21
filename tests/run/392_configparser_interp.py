# configparser BasicInterpolation：%% 转义 / DEFAULT 参与 / 链式递归 / RawConfigParser / write 往返。
import configparser

c = configparser.ConfigParser()
c.read_string("[a]\nfirst = %(second)s\nsecond = %(third)s\nthird = end\npercent = 100%%\nescaped = %%(third)s\nslash = %(third)s/%%plit\nUPPER = u\nref = %(UPPER)s\n")
print(c.get("a", "first"))
print(c.get("a", "second"))
print(c.get("a", "percent"), c.get("a", "escaped"), c.get("a", "slash"))
print(c.get("a", "ref"))
print(c.get("a", "first", vars={"second": "V2"}))
print(c.get("a", "percent", raw=True), c.get("a", "first", raw=True))
print(c.items("a"))

c2 = configparser.ConfigParser()
c2.read_string("[DEFAULT]\nd = %(fromsec)s-base\n[svc]\nfromsec = sv\nval = %(d)s\n")
print(c2.get("svc", "d"))
print(c2.get("svc", "val"))
try:
    c2.get("DEFAULT", "d")
except configparser.InterpolationMissingOptionError as e:
    print("D1", str(e))
print(c2.get("svc", "d", raw=True))

raw = configparser.RawConfigParser()
raw.read_string("[a]\nx = %(y)s\ny = 1\np = 100%%\n")
print(raw.get("a", "x"), raw.get("a", "p"))
print(raw.get("a", "x", raw=True))
print(raw.items("a"))
print(raw.items("a", raw=True))

w = configparser.ConfigParser()
w.read_string("[s]\npct = 50%%\nmulti = a\n  b\n")
f = open("/tmp/pa392_w.ini", "w")
w.write(f)
f.close()
f = open("/tmp/pa392_w.ini", "r")
print(f.read(), end="")
f.close()
w2 = configparser.ConfigParser()
print(w2.read("/tmp/pa392_w.ini"))
print(w2.get("s", "pct"), repr(w2.get("s", "multi")))
print(w2.get("s", "pct", raw=True))
f = open("/tmp/pa392_w.ini", "w")
w2.write(f, False)
f.close()
f = open("/tmp/pa392_w.ini", "r")
print(f.read(), end="")
f.close()
# --- expect ---
# end
# end
# 100% %(third)s end/%plit
# u
# V2
# 100%% %(second)s
# [('first', 'end'), ('second', 'end'), ('third', 'end'), ('percent', '100%'), ('escaped', '%(third)s'), ('slash', 'end/%plit'), ('upper', 'u'), ('ref', 'u')]
# sv-base
# sv-base
# D1 Bad value substitution: option 'd' in section 'DEFAULT' contains an interpolation key 'fromsec' which is not a valid option name. Raw value: '%(fromsec)s-base'
# %(fromsec)s-base
# %(y)s 100%%
# %(y)s
# [('x', '%(y)s'), ('y', '1'), ('p', '100%%')]
# [('x', '%(y)s'), ('y', '1'), ('p', '100%%')]
# [s]
# pct = 50%%
# multi = a
# 	b
#
# ['/tmp/pa392_w.ini']
# 50% 'a\nb'
# 50%%
# [s]
# pct=50%%
# multi=a
# 	b
#
