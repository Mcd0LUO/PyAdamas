# configparser 解析边界 / strict=False / DEFAULT 节操作 / read_dict / 读写往返。
import configparser

c = configparser.ConfigParser(strict=False)
c.read_string("[s]\na = 1\na = 2\n[s]\nb = 3\n")
print(c.sections(), c.options("s"), c.get("s", "a"), c.get("s", "b"))

d = configparser.ConfigParser()
d.read_dict({"one": {"a": "1", "B": "2"}, "two": {"c": "3"}})
print(d.sections(), d.options("one"), d.get("one", "a"), d.get("one", "b"))
d.read_dict({"DEFAULT": {"dd": "9"}, "one": {"a": "1"}})
print(d.defaults(), d.get("one", "dd"), d.get("DEFAULT", "dd"))

e = configparser.ConfigParser()
e.read_string("[DEFAULT]\nd = 1\n[s]\na = 2\n")
print(e.defaults(), e.get("DEFAULT", "d"))
print(e.has_option("", "d"), e.has_option("DEFAULT", "d"), e.has_section("DEFAULT"))
e.set("", "q", "7")
e.set("DEFAULT", "r", "8")
print(e.defaults(), e.get("s", "q"), e.get("s", "r"))
print(e.remove_option("DEFAULT", "d"), e.remove_option("", "q"))
print(e.defaults(), e.options("s"), e.items("s"))

f1 = configparser.ConfigParser()
f1.read_string("[DEFAULT]\nonly = 1\n")
print(f1.sections(), f1.defaults(), f1.has_section("DEFAULT"))
f2 = configparser.ConfigParser()
f2.read_string("")
print(f2.sections(), f2.defaults())
f2.read_string("\n\n")
print(f2.sections())
f2.read_string("[s]\n")
print(f2.sections(), f2.options("s"))

f3 = configparser.ConfigParser()
f3.read_string("[s]\r\na = 1\r\nb = 2\r\n")
print(f3.get("s", "a"), repr(f3.get("s", "b")))

g = configparser.ConfigParser()
g.read_string("[ sp ]\n  spaced key  :  v  \nkey=val:with:colons\nmult = a\n  b\n\n  c\n")
print(g.sections(), g.options(" sp "))
print(g.get(" sp ", "spaced key"), g.get(" sp ", "key"), repr(g.get(" sp ", "mult")))

try:
    h = configparser.ConfigParser()
    h.read_string("[s]\n= v\n")
except configparser.ParsingError as e2:
    print("P1", str(e2))
try:
    h2 = configparser.ConfigParser()
    h2.read_string("[s]\n[]\n")
except configparser.ParsingError as e3:
    print("P2", str(e3))
try:
    h3 = configparser.ConfigParser()
    h3.read_string("[a]\nbad\n", "<mysrc>")
except configparser.ParsingError as e4:
    print("P3", str(e4))
try:
    h4 = configparser.ConfigParser()
    h4.read_string("no header here\n")
except configparser.MissingSectionHeaderError as e5:
    print("P4", str(e5))

t = configparser.ConfigParser()
t.read_string("[t]\na = 1\n\t2\nempty =   \n")
print(repr(t.get("t", "a")), repr(t.get("t", "empty")))
t2 = configparser.ConfigParser()
t2.read_string("[t]\na = 1\n\n")
print(repr(t2.get("t", "a")))

n = configparser.ConfigParser(interpolation=None)
n.read_string("[s]\na = %(b)s\nb = 1\n")
print(n.get("s", "a"), n.get("s", "a", raw=True))
print(n.items("s"))

wd = configparser.ConfigParser()
wd.set("DEFAULT", "k", "v")
f = open("/tmp/pa394d.ini", "w")
wd.write(f)
f.close()
f = open("/tmp/pa394d.ini", "r")
print(repr(f.read()))
f.close()
wr = configparser.ConfigParser()
print(wr.read("/tmp/pa394d.ini"), wr.sections(), wr.defaults(), wr.get("DEFAULT", "k"))
wempty = configparser.ConfigParser()
f = open("/tmp/pa394e.ini", "w")
wempty.write(f)
f.close()
f = open("/tmp/pa394e.ini", "r")
print(repr(f.read()))
f.close()

f = open("/tmp/pa394f.ini", "w")
f.write("[s]\na = 1\n")
f.close()
rf = configparser.ConfigParser()
f = open("/tmp/pa394f.ini", "r")
rf.read_file(f, "<src>")
f.close()
print(rf.sections(), rf.get("s", "a"))
# --- expect ---
# ['s'] ['a', 'b'] 2 3
# ['one', 'two'] ['a', 'b'] 1 2
# {'dd': '9'} 9 9
# {'d': '1'} 1
# True True False
# {'d': '1', 'q': '7', 'r': '8'} 7 8
# True True
# {'r': '8'} ['a', 'r'] [('r', '8'), ('a', '2')]
# [] {'only': '1'} False
# [] {}
# []
# ['s'] []
# 1 '2'
# [' sp '] ['spaced key', 'key', 'mult']
# v val:with:colons 'a\nb\n\nc'
# P1 Source contains parsing errors: '<string>'
# 	[line  2]: '= v\n'
# P2 Source contains parsing errors: '<string>'
# 	[line  2]: '[]\n'
# P3 Source contains parsing errors: '<mysrc>'
# 	[line  2]: 'bad\n'
# P4 File contains no section headers.
# file: '<string>', line: 1
# 'no header here\n'
# '1\n2' ''
# '1'
# %(b)s %(b)s
# [('a', '%(b)s'), ('b', '1')]
# '[DEFAULT]\nk = v\n\n'
# ['/tmp/pa394d.ini'] [] {'k': 'v'} v
# ''
# ['s'] 1
