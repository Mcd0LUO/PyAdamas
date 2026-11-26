# expect: is not defined (deleted above)
# BUG-2（反面）：del 之后在嵌套块里读同一个名字 → 明确拒绝（CPython 是
# NameError；运行期拿不到名字状态，所以在编译期报错，不给静默 0）。
x: int = 5
del x
try:
    print(x)
except NameError:
    print('gone')
