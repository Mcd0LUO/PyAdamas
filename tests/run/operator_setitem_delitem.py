# operator.setitem / operator.delitem（0.516）。与 CPython 3.14.4 逐字节一致。
import operator

xs = [1, 2, 3]
print(operator.setitem(xs, 0, 9), xs)
print(operator.setitem(xs, -1, 8), xs)
d = {'a': 1}
print(operator.setitem(d, 'b', 2), d)
print(operator.setitem(d, 'a', 3), d)
print(operator.delitem(d, 'b'), d)
print(operator.delitem(xs, 0), xs)
print(operator.delitem(xs, -1), xs)
try:
    operator.setitem([1], 5, 9)
except IndexError as e:
    print('IndexError:', e)
try:
    operator.delitem([1], 5)
except IndexError as e:
    print('IndexError:', e)
try:
    operator.delitem({'a': 1}, 'zz')
except KeyError as e:
    print('KeyError:', e)
try:
    operator.setitem((1, 2), 0, 9)
except TypeError as e:
    print('TypeError:', e)
try:
    operator.setitem('ab', 0, 'x')
except TypeError as e:
    print('TypeError:', e)
try:
    operator.setitem(5, 0, 9)
except TypeError as e:
    print('TypeError:', e)
try:
    operator.delitem((1, 2), 0)
except TypeError as e:
    print('TypeError:', e)
try:
    operator.delitem('ab', 0)
except TypeError as e:
    print('TypeError:', e)
try:
    operator.delitem(5, 0)
except TypeError as e:
    print('TypeError:', e)

# --- expect ---
# None [9, 2, 3]
# None [9, 2, 8]
# None {'a': 1, 'b': 2}
# None {'a': 3, 'b': 2}
# None {'a': 3}
# None [2, 8]
# None [2]
# IndexError: list assignment index out of range
# IndexError: list assignment index out of range
# KeyError: 'zz'
# TypeError: 'tuple' object does not support item assignment
# TypeError: 'str' object does not support item assignment
# TypeError: 'int' object does not support item assignment
# TypeError: 'tuple' object doesn't support item deletion
# TypeError: 'str' object doesn't support item deletion
# TypeError: 'int' object does not support item deletion
