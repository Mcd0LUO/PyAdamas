# 内置异常多参构造：args 元组还原、raise→except 与 CPython 一致。
e = ValueError('x', 'y', 'z')
print(len(e.args), e.args[0], e.args[1], e.args[2])

s = ValueError('only')
print(len(s.args), s.args[0])

def f():
    try:
        raise ValueError(10, 'msg', 'path')
    except ValueError as err:
        print('caught', len(err.args), err.args[0], err.args[1], err.args[2])
f()

try:
    raise RuntimeError('a', 'b')
except RuntimeError as r:
    print('rt', len(r.args), r.args[0])

# --- expect ---
# 3 x y z
# 1 only
# caught 3 10 msg path
# rt 2 a
