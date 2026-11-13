# range 现在是惰性对象：只存边界，元素按需算。
r = range(10)
print(len(r))
print(r[0], r[5], r[-1])
print(5 in r, 10 in r, 3 in range(0, 10, 2))
print(list(r))
print(list(range(0, 10, 3)))
print(list(range(10, 0, -2)))
print(list(range(0)))
print(sum(r), min(r), max(r), any(r), all(r))
print(sum(range(5), 10), sum(range(5), 0.5))
print(repr(r), str(r))
print(repr(range(0, 10, 2)), repr(range(10, 0, -1)))
print(range(5) == range(5), range(5) == range(6))

for x in r:
    print(x)

print([x * 2 for x in r])
print(list(x for x in r))
print(sorted(range(5)), list(reversed(range(5))))
print(tuple(range(3)), set(range(3)))
print(list(enumerate(range(2))))
print(list(zip(range(2), range(2))))

def total(xs: list[int]) -> int:
    s = 0
    for x in xs:
        s += x
    return s

print(total(list(range(4))))

# 惰性：大 range 不物化
big = range(1000000000)
print(len(big), big[999999999], sum(range(1000)))

try:
    print(r[10])
except IndexError:
    print("IndexError")

# 异常消息的措辞和 CPython 不同（PyAdamas 的列表路径也是这个措辞），只断言类型
try:
    print(min(range(0)))
except ValueError:
    print("ValueError")

# --- expect ---
# 10
# 0 5 9
# True False False
# [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
# [0, 3, 6, 9]
# [10, 8, 6, 4, 2]
# []
# 45 0 9 True False
# 20 10.5
# range(0, 10) range(0, 10)
# range(0, 10, 2) range(10, 0, -1)
# True False
# 0
# 1
# 2
# 3
# 4
# 5
# 6
# 7
# 8
# 9
# [0, 2, 4, 6, 8, 10, 12, 14, 16, 18]
# [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
# [0, 1, 2, 3, 4] [4, 3, 2, 1, 0]
# (0, 1, 2) {0, 1, 2}
# [(0, 0), (1, 1)]
# [(0, 0), (1, 1)]
# 6
# 1000000000 999999999 499500
# IndexError
# ValueError
