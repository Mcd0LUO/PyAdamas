# operator.index / operator.length_hint（0.513）。与 CPython 3.14.4 逐字节一致。
import operator

print(operator.index(5), operator.index(True), operator.index(-3))
print(operator.length_hint([1, 2, 3]), operator.length_hint({}),
      operator.length_hint("abc"), operator.length_hint((1, 2)))
print(operator.length_hint(1, 7))
print(operator.length_hint(1))   # 没有 len 且没给 default：CPython 返回 0（不是抛）
try:
    operator.index(1.5)
except TypeError as e:
    print("TypeError:", e)
try:
    operator.index("x")
except TypeError as e:
    print("TypeError:", e)

# --- expect ---
# 5 1 -3
# 3 0 3 2
# 7
# 0
# TypeError: 'float' object cannot be interpreted as an integer
# TypeError: 'str' object cannot be interpreted as an integer
