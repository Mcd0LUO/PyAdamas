# is/is not 同一性比较按机器字（0.326 修复）：Optional[str]/unknown 槽
# 与 None 比较不再走 str 内容比较（修复前 NULL 解引用段错误）。
# int()/float() 错误消息 CPython 口径；repr(unknown)；unknown 迭代错误。
def opt(s: str, flag: bool) -> None:
    rv: object = None
    if flag:
        rv = s[1:]
    if rv is None:
        print("none")
    else:
        print(rv)
    if rv is not None:
        print("some", rv)


opt("abc", True)
opt("abc", False)


def narrow(s: str, flag: bool) -> None:
    rv = None
    if flag:
        rv = s
    if not (rv is None):
        print(rv)
    else:
        print("empty")


narrow("x", True)
narrow("x", False)


class Holder:
    v: object

    def __init__(self) -> None:
        self.v = None


h = Holder()
print(h.v is None)
h.v = "s"
print(h.v is None, h.v is not None)

# int/float 错误消息
try:
    int("xx")
except ValueError as e:
    print("I1", str(e))
try:
    int("12x")
except ValueError as e:
    print("I2", str(e))
try:
    int("0x10", 10)
except ValueError as e:
    print("I3", str(e))
print(int("0x10", 16), int("10", 2), int("  42  "))
try:
    float("yy")
except ValueError as e:
    print("F1", str(e))
try:
    float("1.5z")
except ValueError as e:
    print("F2", str(e))

# repr(unknown)
def ident(x: object) -> object:
    return x


print(repr(ident("s")), repr(ident(5)), repr(ident(2.5)))
print(repr(ident([1, 2])), repr(ident({"k": 1})))

# unknown 迭代错误
def iterit(x: object) -> None:
    for v in x:
        print(v)


try:
    iterit(5)
except TypeError as e:
    print("T1", str(e))
try:
    iterit(None)
except TypeError as e:
    print("T2", str(e))
iterit([1, 2])
iterit("ab")
iterit({"k": 1})
# --- expect ---
# bc
# some bc
# none
# x
# empty
# True
# False True
# I1 invalid literal for int() with base 10: 'xx'
# I2 invalid literal for int() with base 10: '12x'
# I3 invalid literal for int() with base 10: '0x10'
# 16 2 42
# F1 could not convert string to float: 'yy'
# F2 could not convert string to float: '1.5z'
# 's' 5 2.5
# [1, 2] {'k': 1}
# T1 'int' object is not iterable
# T2 'NoneType' object is not iterable
# 1
# 2
# a
# b
# k
