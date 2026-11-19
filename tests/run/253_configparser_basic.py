# configparser 基本面：读取/查询/类型转换/DEFAULT/续行/注释/插值/写回。
# 错误消息与 CPython 逐字对齐（见 254）。
import configparser

c = configparser.ConfigParser()
c.read_string("[sec]\nKey = Value\nnum = 42\nflag = yes\npi = 3.5\n[DEFAULT]\nd = base\n")
print(c.sections(), c.options("sec"))
print(c.get("sec", "key"), c.getint("sec", "num"), c.getfloat("sec", "pi"))
print(c.getboolean("sec", "flag") == True)
print(c.items("sec"))
print(c.get("sec", "d"))
print(c.has_option("sec", "KEY"), c.has_section("sec"), c.has_section("nope"))
print(c.get("sec", "a", fallback="fb"), c.getint("sec", "a", fallback=7))
print(c.get("sec", "num", raw=True))

# 布尔各形态
c2 = configparser.ConfigParser()
c2.read_string("[b]\ny1 = 1\ny2 = YES\ny3 = True\ny4 = on\nn1 = 0\nn2 = no\nn3 = FALSE\nn4 = Off\n")
for k in ["y1", "y2", "y3", "y4"]:
    print(c2.getboolean("b", k) == True)
for k in ["n1", "n2", "n3", "n4"]:
    print(c2.getboolean("b", k) == False)

# 续行 / 注释 / 空值 / 冒号 / 行内注释保留
c3 = configparser.ConfigParser()
c3.read_string("[s]\nmulti = line1\n  line2\n\tline3\n; comment\n# comment2\nempty =\ncolon: cv\neqmore = a = b\ninline = 1 ; keep\n")
print(repr(c3.get("s", "multi")), repr(c3.get("s", "empty")))
print(repr(c3.get("s", "colon")), repr(c3.get("s", "eqmore")), repr(c3.get("s", "inline")))
c4 = configparser.ConfigParser()
c4.read_string("[s]\na = 1\n\n  2\n")
print(repr(c4.get("s", "a")))

# 大小写：节敏感、选项小写化
c5 = configparser.ConfigParser()
c5.read_string("[S]\nUPPER = 1\n")
print(c5.get("S", "upper"), c5.options("S"))
print(c5.remove_option("S", "upper"), c5.remove_option("S", "upper"),
      c5.remove_section("S"), c5.remove_section("S"))

# 插值
c6 = configparser.ConfigParser()
c6.read_string("[s]\na = %(b)s-x\nb = hello\n")
print(c6.get("s", "a"))
c7 = configparser.ConfigParser()
c7.read_string("[s]\na = %(b)s\nb = %(c)s\nc = deep\n")
print(c7.get("s", "a"))
c8 = configparser.ConfigParser()
c8.read_string("[s]\na = 50%%\n")
print(c8.get("s", "a"))
c9 = configparser.ConfigParser(interpolation=None)
c9.read_string("[s]\na = %(b)s\nb = x\n")
print(c9.get("s", "a"))
print(c9.get("s", "a", raw=True))

# DEFAULT 共享与 vars
c10 = configparser.ConfigParser()
c10.read_string("[DEFAULT]\nd = 1\n[s]\na = 2\n")
print(c10.defaults(), c10.options("s"), c10.items("s"))
print(c10.get("s", "D"))
c11 = configparser.ConfigParser()
c11.read_string("[s]\na = %(v)s\n")
print(c11.get("s", "a", vars={"v": "ext"}))

# set / add_section / write 往返
c12 = configparser.ConfigParser()
c12.read_string("[s]\na = 1\n")
c12.set("s", "b", "2")
c12.add_section("t")
c12.set("t", "x", "y")
f = open("/tmp/pa253_cfg.ini", "w")
c12.write(f)
f.close()
f = open("/tmp/pa253_cfg.ini", "r")
print(f.read(), end="")
f.close()
c13 = configparser.ConfigParser()
print(c13.read("/tmp/pa253_cfg.ini"))
print(c13.get("s", "a"), c13.get("s", "b"), c13.get("t", "x"))
print(c13.read("/nonexistent/file.ini"))

# read_file / read_string source
c14 = configparser.ConfigParser()
f = open("/tmp/pa253_cfg.ini", "r")
c14.read_file(f)
f.close()
print(c14.sections())

# 写回时 %% 再转义
c15 = configparser.ConfigParser()
c15.read_string("[s]\na = 50%%\n")
f = open("/tmp/pa253_cfg2.ini", "w")
c15.write(f)
f.close()
f = open("/tmp/pa253_cfg2.ini", "r")
print(f.read(), end="")
f.close()

# 多次 read 合并
c16 = configparser.ConfigParser()
c16.read_string("[s]\na = 1\n")
c16.read_string("[s]\nb = 2\n[t]\nc = 3\n")
print(c16.sections(), c16.get("s", "a"), c16.get("s", "b"), c16.get("t", "c"))

# optionxform 覆盖
class KeepCase(configparser.ConfigParser):
    def optionxform(self, optionstr: str) -> str:
        return optionstr
kc = KeepCase()
kc.read_string("[s]\nMiXeD = 1\n")
print(kc.get("s", "MiXeD"))
# --- expect ---
# ['sec'] ['key', 'num', 'flag', 'pi', 'd']
# Value 42 3.5
# True
# [('d', 'base'), ('key', 'Value'), ('num', '42'), ('flag', 'yes'), ('pi', '3.5')]
# base
# True True False
# fb 7
# 42
# True
# True
# True
# True
# True
# True
# True
# True
# 'line1\nline2\nline3' ''
# 'cv' 'a = b' '1 ; keep'
# '1\n\n2'
# 1 ['upper']
# True False True False
# hello-x
# deep
# 50%
# %(b)s
# %(b)s
# {'d': '1'} ['a', 'd'] [('d', '1'), ('a', '2')]
# 1
# ext
# [s]
# a = 1
# b = 2
#
# [t]
# x = y
#
# ['/tmp/pa253_cfg.ini']
# 1 2 y
# []
# ['s', 't']
# [s]
# a = 50%%
#
# ['s', 't'] 1 2 3
# 1
