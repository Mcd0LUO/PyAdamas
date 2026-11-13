from operator import add, sub, mul, pow, truediv, floordiv, mod
from operator import eq, ne, lt, le, gt, ge
from operator import neg, abs, invert
from operator import truth, not_
from operator import concat, contains, getitem
from operator import itemgetter
import operator

print(add(2, 3), sub(2, 3), mul(2, 3), pow(2, 10))
print(truediv(7, 2), floordiv(7, 2), mod(7, 2))
print(add(1.5, 2.25), mul(2.0, 3))
print(add("ab", "cd"), concat("x", "y"))
print(eq(1, 1), ne(1, 1), lt(1, 2), le(2, 2), gt(1, 2), ge(2, 2))
print(eq("a", "a"), lt("a", "b"))
print(neg(5), abs(-7), abs(-2.5))
print(invert(5), invert(0))
print(truth(0), truth(3), not_(0), not_(4))
print(operator.is_(None, None), operator.is_not(1, 2))
print(operator.neg(-3), operator.pos(4))
print(contains([1, 2, 3], 2), contains({"a": 1}, "a"), contains("abcd", "bc"))
print(getitem([10, 20, 30], 1), getitem({"k": 5}, "k"), getitem("xyz", 2))
print(getitem((7, 8), 0))

g0 = itemgetter(0)
g1 = itemgetter(1)
print(g0([10, 20]))
print(g1(("a", "b")))
pair = itemgetter(0, 2)
print(pair([1, 2, 3]))
print(operator.add(4, 5), operator.lt(4, 5), operator.truth(7))

# --- expect ---
# 5 -1 6 1024
# 3.5 3 1
# 3.75 6.0
# abcd xy
# True False True True False True
# True True
# -5 7 2.5
# -6 -1
# False True True False
# True True
# 3 4
# True True True
# 20 5 z
# 7
# 10
# b
# (1, 3)
# 9 True True
