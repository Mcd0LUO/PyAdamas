# BUG-2：del 之后的读取。0.537 的保守版 del 只在「同一个 statements 序列」里
# 可见，于是 `del x` 之后在 try/if/for 体里读 x 检测不到，降级层把槽清零后
# 静默打印 0（CPython 是 NameError）。修复后嵌套块继承外层的删除集合。
# 这个用例覆盖「不能误杀」的那一半：块内重新赋值（复活）、del 不出块、
# 函数体是新作用域。确定性删除的拒绝口径见 tests/strict/del_after_del_nested.py。
a: int = 1
del a
if True:
    a = 2
print(a)

b: int = 1
del b
def use_global() -> int:
    return b
b = 3
print(use_global())

c: int = 1
del c
for i in range(3):
    c = i
print(c)

d: int = 1
del d
while True:
    d = 5
    break
print(d)

e: int = 1
del e
try:
    e = 9
except Exception:
    pass
print(e)

f: int = 1
if f > 0:
    del f
print('branch del ok')

g: int = 7
del g
g = 8
print(g)

# --- expect ---
# 2
# 3
# 2
# 5
# 9
# branch del ok
# 8
