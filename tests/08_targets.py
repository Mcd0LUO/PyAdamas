# 赋值目标、循环、with、闭包 —— 解析器最容易搞错形状的一类语法
#
# 固定住这几处修复：
#   1) for a, *b in xs：星号目标不能走 namedexpr，否则 in 被当比较运算吃掉
#   2) with (open('a') as f, open('b') as g)：括号项列表（PEP 617）
#   3) match, other = 1, 2：软关键字 match 不能被当成 match 语句
#   4) for x in (1, "a")：迭代元素类型取公共上界

pairs = [(1, "a"), (2, "b")]
for num, text in pairs:
    print(num, text)

rows = [[1, 2, 3, 4]]
for first, *middle, last in rows:
    print(first, middle, last)

head, *tail = [1, 2, 3]
*init, final = (1, 2, 3)
[a, b] = [10, 20]
(p, q) = (1, 2)

nested_target = [[1, 2], [3, 4]]
for (left, right) in nested_target:
    print(left + right)

# 软关键字：match 作为普通名字
match, other = 1, 2
match = match + other
case = 3
print(match, case)

# 括号 with 项列表 + 续行
with open("a.txt") as fa, open("b.txt") as fb:
    pass

with (open("a.txt") as fa,
      open("b.txt") as fb):
    pass

with (
    open("a.txt") as fa,
    open("b.txt") as fb,
):
    pass

# 条件赋值、海象、链式比较
limit = 10
clamped = limit if limit < 100 else 100
if (found := limit * 2) > 10:
    print(found)

# 闭包与 nonlocal / global
counter = 0


def bump() -> int:
    global counter
    counter = counter + 1
    return counter


def make_adder(base: int):
    def add(v: int) -> int:
        return base + v

    def add_all(values: list[int]) -> int:
        total = 0
        for v in values:
            total = total + add(v)
        return total

    return add_all


print(bump(), make_adder(3)([1, 2, 3]))

# 嵌套函数引用外层稍后赋值的变量（名字在调用时解析）
def uses_later() -> int:
    def inner() -> int:
        return later_value
    later_value = 7
    return inner()


# try/except/else/finally 各种形状
def risky(v: int) -> int:
    try:
        if v == 0:
            raise ValueError("zero")
        result = 100 // v
    except (ValueError, ZeroDivisionError) as exc:
        print(exc)
        result = -1
    except Exception:
        result = -2
    else:
        result = result + 1
    finally:
        print("done")
    return result


# 推导式的多层 for 与条件
matrix = [[1, 2], [3, 4]]
flat = [v for row in matrix for v in row if v > 1]
index = {v: i for i, v in enumerate(flat)}
uniq = {v % 3 for v in flat}
gen = (v * 2 for v in flat)

# 解包实参与关键字展开
args = [1, 2, 3]
kwargs = {"sep": "-"}


def join3(a: int, b: int, c: int, sep: str = "") -> str:
    return f"{a}{sep}{b}{sep}{c}"


print(join3(*args, **kwargs))
