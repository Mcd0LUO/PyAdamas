# configparser 核心面：解析 / 查询 / DEFAULT 继承 / fallback / vars / read 多文件。
import configparser

TEXT = (
    "[general] trailing junk\n"
    "Name = Alice\n"
    "age: 30\n"
    "ratio = 2.5\n"
    "enabled = yes\n"
    "note = hello = world\n"
    "semi = v ; kept\n"
    "hash = v # kept\n"
    "multi = line1\n"
    "   line2\n"
    "\tline3\n"
    "withblank = a\n"
    "\n"
    "   b\n"
    "aftercomment = x\n"
    "; full line comment\n"
    "# another\n"
    "   y\n"
    "empty =\n"
    "[other]\n"
    "x = 1\n"
)
c = configparser.ConfigParser()
c.read_string(TEXT)
print(c.sections())
print(c.options("general"))
print(c.get("general", "name"), c.getint("general", "age"), c.getfloat("general", "ratio"))
print(c.getboolean("general", "enabled"))
print(repr(c.get("general", "note")), repr(c.get("general", "semi")), repr(c.get("general", "hash")))
print(repr(c.get("general", "multi")), repr(c.get("general", "withblank")))
print(repr(c.get("general", "aftercomment")), repr(c.get("general", "empty")))
print(c.items("general"))

c2 = configparser.ConfigParser()
c2.read_string("[DEFAULT]\nbase = fallback\nnum = 7\n[svc]\nhost = h1\nname = %(base)s-%(host)s\n")
print(c2.defaults(), c2.sections())
print(c2.options("svc"))
print(c2.items("svc"))
print(c2.get("svc", "base"), c2.get("DEFAULT", "base"))
print(c2.has_option("svc", "base"), c2.has_option("DEFAULT", "base"), c2.has_option("nope", "base"))
print(c2.has_option("svc", "nope"), c2.has_option("", "base"))

print(c2.get("svc", "missing", fallback=None) == None)
print(c2.get("svc", "missing", fallback="fb"))
print(c2.getint("svc", "missing", fallback=5))
print(c2.getfloat("svc", "missing", fallback=1.5))
print(c2.getboolean("svc", "missing", fallback=False))
print(c2.get("nope", "x", fallback="ns"))

c3 = configparser.ConfigParser()
c3.read_string("[s]\na = %(b)s\nb = sec\n")
print(c3.get("s", "a"))
print(c3.get("s", "a", vars={"b": "ext"}))
print(c3.get("s", "b", vars={"b": "ext"}))
print(c3.get("s", "onlyvar", vars={"onlyvar": "ov"}))
print(c3.items("s", vars={"b": "ext"}))
print(c3.get("s", "a", raw=True))
print(c3.items("s", raw=True))

f = open("/tmp/pa390_a.ini", "w")
f.write("[one]\nk = 1\n")
f.close()
f = open("/tmp/pa390_b.ini", "w")
f.write("[two]\nk = 2\n")
f.close()
c4 = configparser.ConfigParser()
print(c4.read(["/tmp/pa390_a.ini", "/tmp/pa390_missing.ini", "/tmp/pa390_b.ini"]))
print(c4.sections(), c4.get("one", "k"), c4.get("two", "k"))
f = open("/tmp/pa390_a.ini", "r")
c5 = configparser.ConfigParser()
c5.read_file(f)
f.close()
print(c5.sections(), c5.get("one", "k"))

c6 = configparser.ConfigParser(defaults={"d": "1", "n": "2"})
print(c6.defaults(), c6.sections(), c6.has_option("", "d"))
c6.read_string("[s]\na = %(d)s\n")
print(c6.get("s", "a"), c6.getint("s", "d"))

class KeepCase(configparser.ConfigParser):
    def optionxform(self, optionstr: str) -> str:
        return optionstr
kc = KeepCase()
kc.read_string("[S]\nMiXeD = 1\n")
print(kc.sections(), kc.options("S"), kc.get("S", "MiXeD"))

c7 = configparser.ConfigParser()
c7.read_string("[s]\na = 1\n")
c7.read_string("[s]\na = 2\nb = 3\n")
print(c7.sections(), c7.get("s", "a"), c7.get("s", "b"), c7.options("s"))

c8 = configparser.ConfigParser()
c8.read_string("[DEFAULT]\nd = 1\n[DEFAULT]\ne = 2\n[s]\na = 3\n")
print(c8.defaults(), c8.get("s", "e"), c8.sections())
# --- expect ---
# ['general', 'other']
# ['name', 'age', 'ratio', 'enabled', 'note', 'semi', 'hash', 'multi', 'withblank', 'aftercomment', 'empty']
# Alice 30 2.5
# True
# 'hello = world' 'v ; kept' 'v # kept'
# 'line1\nline2\nline3' 'a\n\nb'
# 'x\ny' ''
# [('name', 'Alice'), ('age', '30'), ('ratio', '2.5'), ('enabled', 'yes'), ('note', 'hello = world'), ('semi', 'v ; kept'), ('hash', 'v # kept'), ('multi', 'line1\nline2\nline3'), ('withblank', 'a\n\nb'), ('aftercomment', 'x\ny'), ('empty', '')]
# {'base': 'fallback', 'num': '7'} ['svc']
# ['host', 'name', 'base', 'num']
# [('base', 'fallback'), ('num', '7'), ('host', 'h1'), ('name', 'fallback-h1')]
# fallback fallback
# True True False
# False True
# True
# fb
# 5
# 1.5
# False
# ns
# sec
# ext
# ext
# ov
# [('a', 'ext'), ('b', 'ext')]
# %(b)s
# [('a', '%(b)s'), ('b', 'sec')]
# ['/tmp/pa390_a.ini', '/tmp/pa390_b.ini']
# ['one', 'two'] 1 2
# ['one'] 1
# {'d': '1', 'n': '2'} [] True
# 1 1
# ['S'] ['MiXeD'] 1
# ['s'] 2 3 ['a', 'b']
# {'d': '1', 'e': '2'} 2 ['s']
