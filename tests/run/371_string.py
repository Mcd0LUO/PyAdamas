# 371_string —— stdlib/string.py 与 CPython 对拍（常量/Template/capwords）
#
# 覆盖面：9 个常量、Template（$$ 转义、$name、braced、substitute/
# safe_substitute/is_valid/get_identifiers、KeyError 与 ValueError 行列口径）；
# capwords（默认空白切分/自定义 sep/sep="" 的 ValueError）。
# 期望输出由 CPython 3.14.4 string 模块逐行生成（见文件尾）。
# 边界：从 mapping 取出的值只有 8B 位模式，float/None/bool 的 str() 与
# CPython 不同（容器元素无逐元素 tag），用例只用 str/int 取值。
import string

print(string.ascii_lowercase)
print(string.ascii_uppercase)
print(string.ascii_letters)
print(string.digits, string.hexdigits, string.octdigits)
print(string.punctuation)
print(len(string.whitespace), len(string.printable), len(string.punctuation))
print(string.whitespace == ' ' + chr(9) + chr(10) + chr(13) + chr(11) + chr(12))
print(string.printable == string.digits + string.ascii_letters + string.punctuation + string.whitespace)
print(string.printable[:36])

# Template: $$ escape / $name / braced / safe / is_valid / get_identifiers
t = string.Template('$a and ${b} and $$')
print(repr(t.substitute({'a': 'A', 'b': 'B'})))
print(repr(t.safe_substitute({'a': 'A'})))
print(t.get_identifiers())
print(t.is_valid())
print(repr(string.Template('$n').substitute({'n': 3})))
print(repr(string.Template('$s').substitute({'s': 'txt'})))
print(repr(string.Template('$Foo').substitute({'Foo': 'x'})))
print(repr(string.Template('${_x9}').substitute({'_x9': 'u'})))
print(string.Template('$a$').is_valid(), string.Template('$a$').get_identifiers())
print(string.Template('$$').is_valid(), repr(string.Template('$$').substitute({})))
print(string.Template('$a ${b} $a').get_identifiers())
print(string.Template('no placeholders').is_valid())
print(repr(string.Template('text $$$${a}').substitute({'a': 'V'})))
print(repr(string.Template('$a').safe_substitute({})))
print(repr(string.Template('${1}').safe_substitute({})))
print(repr(string.Template('a$').safe_substitute({'a': 1})))

# 错误口径
try:
    string.Template('$a').substitute({})
except KeyError:
    print('KeyError')
bad_list = ['$', 'a $', '$1', '${1}', 'a$', '\n$', 'a\n  $', '$-']
for bad in bad_list:
    try:
        string.Template(bad).substitute({'a': 1})
    except ValueError as e:
        print('ValueError:', e)
    print(string.Template(bad).is_valid())

# capwords
print(repr(string.capwords(' aBc  dEf ')))
print(repr(string.capwords('hello world')))
print(repr(string.capwords('a,b,C', ',')))
print(repr(string.capwords('one-two', '-')))
print(repr(string.capwords('  ')))
print(repr(string.capwords('')))
print(repr(string.capwords('A B  C')))
try:
    string.capwords('a b', '')
except ValueError as e:
    print('ValueError:', e)
# --- expect ---
# abcdefghijklmnopqrstuvwxyz
# ABCDEFGHIJKLMNOPQRSTUVWXYZ
# abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ
# 0123456789 0123456789abcdefABCDEF 01234567
# !"#$%&'()*+,-./:;<=>?@[\]^_`{|}~
# 6 100 32
# True
# True
# 0123456789abcdefghijklmnopqrstuvwxyz
# 'A and B and $'
# 'A and ${b} and $'
# ['a', 'b']
# True
# '3'
# 'txt'
# 'x'
# 'u'
# False ['a']
# True '$'
# ['a', 'b']
# True
# 'text $${a}'
# '$a'
# '${1}'
# 'a$'
# KeyError
# ValueError: Invalid placeholder in string: line 1, col 1
# False
# ValueError: Invalid placeholder in string: line 1, col 3
# False
# ValueError: Invalid placeholder in string: line 1, col 1
# False
# ValueError: Invalid placeholder in string: line 1, col 1
# False
# ValueError: Invalid placeholder in string: line 1, col 2
# False
# ValueError: Invalid placeholder in string: line 2, col 1
# False
# ValueError: Invalid placeholder in string: line 2, col 3
# False
# ValueError: Invalid placeholder in string: line 1, col 1
# False
# 'Abc Def'
# 'Hello World'
# 'A,B,C'
# 'One-Two'
# ''
# ''
# 'A B C'
# ValueError: empty separator
