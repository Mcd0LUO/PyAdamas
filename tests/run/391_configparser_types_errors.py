# configparser 类型化取值 + 错误类型/消息（CPython 功能对拍）。
import configparser

c = configparser.ConfigParser()
c.read_string("[typed]\nnum = 42\nneg = -3\nreal = 3.5\nexp = 1e3\nyes1 = 1\nyes2 = YES\nyes3 = True\nyes4 = on\nno1 = 0\nno2 = no\nno3 = FALSE\nno4 = Off\nbad = maybe\nval = 7\n")

print(c.getint("typed", "num"))
print(c.getint("typed", "neg"))
print(c.getfloat("typed", "real"))
print(c.getfloat("typed", "exp"))
print(c.getboolean("typed", "yes1"), c.getboolean("typed", "yes2"), c.getboolean("typed", "yes3"), c.getboolean("typed", "yes4"))
print(c.getboolean("typed", "no1"), c.getboolean("typed", "no2"), c.getboolean("typed", "no3"), c.getboolean("typed", "no4"))
print(c.getint("typed", "missing", fallback="notint"))
print(c.getboolean("typed", "missing", fallback=True))

try:
    c.getint("typed", "val", fallback=7)
except ValueError as e:
    print("V1", str(e))
try:
    c.getint("typed", "bad")
except ValueError as e:
    print("V2", str(e))
try:
    c.getfloat("typed", "bad")
except ValueError as e:
    print("V3", str(e))
try:
    c.getboolean("typed", "bad")
except ValueError as e:
    print("V4", str(e))
try:
    c.getboolean("typed", "bad", fallback=False)
except ValueError as e:
    print("V5", str(e))

try:
    c.get("nope", "x")
except configparser.NoSectionError as e:
    print("E1", str(e))
try:
    c.get("typed", "nope")
except configparser.NoOptionError as e:
    print("E2", str(e))
try:
    c.options("nope")
except configparser.NoSectionError as e:
    print("E3", str(e))
try:
    c.items("nope")
except configparser.NoSectionError as e:
    print("E4", str(e))
try:
    c.set("nope", "a", "b")
except configparser.NoSectionError as e:
    print("E5", str(e))
try:
    c.remove_option("nope", "a")
except configparser.NoSectionError as e:
    print("E6", str(e))
try:
    c.add_section("typed")
except configparser.DuplicateSectionError as e:
    print("E7", str(e))
try:
    c.add_section("DEFAULT")
except ValueError as e:
    print("E8", str(e))

d = configparser.ConfigParser()
try:
    d.read_string("[a]\nx = 1\nx = 2\n")
except configparser.DuplicateOptionError as e:
    print("E9", str(e))
try:
    d.read_string("[a]\n[a]\nx = 1\n")
except configparser.DuplicateSectionError as e:
    print("E10", str(e))
try:
    d.read_string("x = 1\n")
except configparser.MissingSectionHeaderError as e:
    print("E11", str(e))
try:
    d.read_string("[a]\nbad line\nmore bad\n")
except configparser.ParsingError as e:
    print("E12", str(e))

p = configparser.ConfigParser()
p.read_string("[q]\na = %(missing)s\n")
try:
    p.get("q", "a")
except configparser.InterpolationMissingOptionError as e:
    print("I1", str(e))
p2 = configparser.ConfigParser()
p2.read_string("[q]\na = %(a)s\n")
try:
    p2.get("q", "a")
except configparser.InterpolationDepthError as e:
    print("I2", str(e))
p3 = configparser.ConfigParser()
p3.read_string("[q]\na = 100%\n")
try:
    p3.get("q", "a")
except configparser.InterpolationSyntaxError as e:
    print("I3", str(e))
p4 = configparser.ConfigParser()
p4.read_string("[q]\na = %()s\n")
try:
    p4.get("q", "a")
except configparser.InterpolationSyntaxError as e:
    print("I4", str(e))
p5 = configparser.ConfigParser()
p5.read_string("[q]\na = %(b)s\nb = %(a)s\n")
try:
    p5.get("q", "a")
except configparser.InterpolationDepthError as e:
    print("I5", str(e))
p6 = configparser.ConfigParser()
p6.read_string("[q]\na = 1\n")
try:
    p6.set("q", "b", "bad%")
except ValueError as e:
    print("I6", str(e))
try:
    p6.set("q", "b", "ok%(x)s")
except ValueError as e:
    print("I7", str(e))
pr = configparser.RawConfigParser()
pr.read_string("[q]\na = %(b)s\nb = x\n")
print(pr.get("q", "a"))

try:
    raise configparser.NoSectionError("x")
except configparser.Error:
    print("h1")
try:
    raise configparser.MissingSectionHeaderError("f", 1, "x\n")
except configparser.ParsingError:
    print("h2")
try:
    raise configparser.InterpolationMissingOptionError("a", "s", "%(zz)s", "zz")
except configparser.InterpolationError:
    print("h3")
try:
    raise configparser.Error("x")
except Exception:
    print("h4")
try:
    raise configparser.InvalidWriteError("x")
except configparser.Error:
    print("h5")
# --- expect ---
# 42
# -3
# 3.5
# 1000.0
# True True True True
# False False False False
# notint
# True
# V2 invalid literal for int() with base 10: 'maybe'
# V3 could not convert string to float: 'maybe'
# V4 Not a boolean: maybe
# V5 Not a boolean: maybe
# E1 No section: 'nope'
# E2 No option 'nope' in section: 'typed'
# E3 No section: 'nope'
# E4 No section: 'nope'
# E5 No section: 'nope'
# E6 No section: 'nope'
# E7 Section 'typed' already exists
# E8 Invalid section name: 'DEFAULT'
# E9 While reading from '<string>' [line  3]: option 'x' in section 'a' already exists
# E10 While reading from '<string>' [line  2]: section 'a' already exists
# E11 File contains no section headers.
# file: '<string>', line: 1
# 'x = 1\n'
# E12 Source contains parsing errors: '<string>'
# 	[line  2]: 'bad line\n'
# 	[line  3]: 'more bad\n'
# I1 Bad value substitution: option 'a' in section 'q' contains an interpolation key 'missing' which is not a valid option name. Raw value: '%(missing)s'
# I2 Recursion limit exceeded in value substitution: option 'a' in section 'q' contains an interpolation key which cannot be substituted in 10 steps. Raw value: '%(a)s'
# I3 '%' must be followed by '%' or '(', found: '%'
# I4 bad interpolation variable reference '%()s'
# I5 Recursion limit exceeded in value substitution: option 'a' in section 'q' contains an interpolation key which cannot be substituted in 10 steps. Raw value: '%(b)s'
# I6 invalid interpolation syntax in 'bad%' at position 3
# %(b)s
# h1
# h2
# h3
# h4
# h5
