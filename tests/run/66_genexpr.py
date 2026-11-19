# 生成器表达式：惰性求值、一次性消费、内置函数流式消费
def src(tag, xs: list[int]) -> list[int]:
    print(tag)
    return xs

# 最外层 iterable 在创建时求值，其余部分等 next 才跑
g = (x * 2 for x in src("src", [1, 2, 3]))
print("created")
for v in g:
    print("got", v)
# 一次性消费：第二次迭代为空
print(len(list(g)))


def pulls(n):
    for i in range(n):
        print("pull", i)
        yield i


# 短路：命中就停，后面的元素连算都不算
print(any(x > 1 for x in pulls(4)))
print(all(x < 2 for x in pulls(4)))
print(sum(x for x in pulls(3)))
print(min(x for x in pulls(3)))
print(max(x for x in pulls(3)))

# 内置函数消费生成器
print(list(x + 1 for x in [1, 2]))
print(tuple(x for x in "ab"))
print(sorted(x for x in [3, 1, 2]))
print(len(set(x % 2 for x in [1, 2, 3])))
print(list(enumerate(x * 10 for x in [1, 2])))
print(list(zip((x for x in [1, 2]), (y for y in [3, 4]))))
print(",".join(str(x) for x in [1, 2, 3]))

# 过滤与多层 for
print(sum(x for x in range(10) if x % 2 == 0 if x > 2))
print(sum(x * y for x in [1, 2] for y in [10, 20]))

# 生成器套生成器
print(sum(y for y in (x * x for x in range(4))))


# 闭包捕获
def scale(n):
    return (i * n for i in range(3))


print(list(scale(10)))
print(next(scale(2)))


def total(it):
    return sum(it)


print(total(x + 1 for x in [1, 2, 3]))

# 推导式消费生成器
print([x for x in (y * 2 for y in range(4)) if x > 2])


def outer(n):
    yield sum(x * n for x in range(3))


print(list(outer(2)))

# 元素类型跟着合成函数的形参回填走：字符串的 min/max/any/all 要按真值
# 和字典序判断，不能退化成指针比较
print(min(x for x in ["bb", "a", "ccc"]))
print(max(x for x in ["bb", "a", "ccc"]))
print(any(x for x in ["", "a"]))
print(all(x for x in ["a", ""]))

# 丢弃生成器：跨 yield 的 finally 要跑
def cleanup_gen():
    try:
        yield 1
        yield 2
    finally:
        print("cleanup")


cg = cleanup_gen()
print(next(cg))
cg = None
print("after drop")
# --- expect ---
# src
# created
# got 2
# got 4
# got 6
# 0
# pull 0
# pull 1
# pull 2
# True
# pull 0
# pull 1
# pull 2
# False
# pull 0
# pull 1
# pull 2
# 3
# pull 0
# pull 1
# pull 2
# 0
# pull 0
# pull 1
# pull 2
# 2
# [2, 3]
# ('a', 'b')
# [1, 2, 3]
# 2
# [(0, 10), (1, 20)]
# [(1, 3), (2, 4)]
# 1,2,3
# 18
# 90
# 14
# [0, 10, 20]
# 0
# 9
# [4, 6]
# [6]
# a
# ccc
# True
# False
# 1
# cleanup
# after drop
