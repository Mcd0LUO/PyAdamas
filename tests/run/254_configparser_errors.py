# configparser 错误路径：异常类型与消息逐字对拍 CPython。
import configparser

c = configparser.ConfigParser()
c.read_string("[sec]\nKey = Value\nnum = 42\n")

try:
    c.get("nope", "x")
except configparser.NoSectionError as e:
    print("E1", str(e))
try:
    c.get("sec", "nope")
except configparser.NoOptionError as e:
    print("E2", str(e))
try:
    c.add_section("sec")
except configparser.DuplicateSectionError as e:
    print("E3", str(e))
try:
    c.add_section("DEFAULT")
except ValueError as e:
    print("E4", str(e))
try:
    c.read_string("[a]\nx=1\nx=2\n")
except configparser.DuplicateOptionError as e:
    print("E5", str(e))
try:
    c.read_string("[a]\n[a]\nx=1\n")
except configparser.DuplicateSectionError as e:
    print("E6", str(e))
try:
    c.read_string("x=1\n")
except configparser.MissingSectionHeaderError as e:
    print("E7", str(e))
try:
    c.read_string("[a]\nbadline\n")
except configparser.ParsingError as e:
    print("E8", str(e))
try:
    c.read_string("[a]\nbad1\nbad2\n")
except configparser.ParsingError as e:
    print("E9", str(e))
try:
    c.getboolean("sec", "key")
except ValueError as e:
    print("E10", str(e))
try:
    c.getint("sec", "key")
except ValueError as e:
    print("E11", str(e))
try:
    c.getfloat("sec", "key")
except ValueError as e:
    print("E12", str(e))
try:
    c.options("nope")
except configparser.NoSectionError as e:
    print("E13", str(e))
try:
    c.items("nope")
except configparser.NoSectionError as e:
    print("E14", str(e))
try:
    c.set("nope", "a", "b")
except configparser.NoSectionError as e:
    print("E15", str(e))
try:
    c.remove_option("nope", "a")
except configparser.NoSectionError as e:
    print("E16", str(e))

# 插值错误
try:
    c3 = configparser.ConfigParser()
    c3.read_string("[s]\na = %(zz)s\n")
    c3.get("s", "a")
except configparser.InterpolationMissingOptionError as e:
    print("E17", str(e))
try:
    c4 = configparser.ConfigParser()
    c4.read_string("[s]\na = %(a)s\n")
    c4.get("s", "a")
except configparser.InterpolationDepthError as e:
    print("E18", str(e))
try:
    c5 = configparser.ConfigParser()
    c5.read_string("[s]\na = 100%\n")
    c5.get("s", "a")
except configparser.InterpolationSyntaxError as e:
    print("E19", str(e))
try:
    c6 = configparser.ConfigParser()
    c6.read_string("[s]\na = %(b)s\nb = %(a)s\n")
    c6.get("s", "a")
except configparser.InterpolationDepthError as e:
    print("E20", str(e))
try:
    c7 = configparser.ConfigParser()
    c7.read_string("[s]\na = 1\n")
    c7.set("s", "b", "bad%")
except ValueError as e:
    print("E21", str(e))
try:
    c8 = configparser.ConfigParser()
    c8.read_string("[s]\na = %()s\n")
    c8.get("s", "a")
except configparser.InterpolationSyntaxError as e:
    print("E22", str(e))

# 异常层级（用 except 匹配验证）
try:
    raise configparser.NoSectionError("x")
except configparser.Error:
    print(True)
try:
    raise configparser.MissingSectionHeaderError("f", 1, "x\n")
except configparser.ParsingError:
    print(True)
try:
    raise configparser.InterpolationMissingOptionError("a", "s", "%(zz)s", "zz")
except configparser.InterpolationError:
    print(True)
try:
    raise configparser.Error("x")
except Exception:
    print(True)
# --- expect ---
# E1 No section: 'nope'
# E2 No option 'nope' in section: 'sec'
# E3 Section 'sec' already exists
# E4 Invalid section name: 'DEFAULT'
# E5 While reading from '<string>' [line  3]: option 'x' in section 'a' already exists
# E6 While reading from '<string>' [line  2]: section 'a' already exists
# E7 File contains no section headers.
# file: '<string>', line: 1
# 'x=1\n'
# E8 Source contains parsing errors: '<string>'
# 	[line  2]: 'badline\n'
# E9 Source contains parsing errors: '<string>'
# 	[line  2]: 'bad1\n'
# 	[line  3]: 'bad2\n'
# E10 Not a boolean: Value
# E11 invalid literal for int() with base 10: 'Value'
# E12 could not convert string to float: 'Value'
# E13 No section: 'nope'
# E14 No section: 'nope'
# E15 No section: 'nope'
# E16 No section: 'nope'
# E17 Bad value substitution: option 'a' in section 's' contains an interpolation key 'zz' which is not a valid option name. Raw value: '%(zz)s'
# E18 Recursion limit exceeded in value substitution: option 'a' in section 's' contains an interpolation key which cannot be substituted in 10 steps. Raw value: '%(a)s'
# E19 '%' must be followed by '%' or '(', found: '%'
# E20 Recursion limit exceeded in value substitution: option 'a' in section 's' contains an interpolation key which cannot be substituted in 10 steps. Raw value: '%(b)s'
# E21 invalid interpolation syntax in 'bad%' at position 3
# E22 bad interpolation variable reference '%()s'
# True
# True
# True
# True
