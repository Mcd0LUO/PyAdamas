# 类装饰器：在类定义点拿到运行期类对象，返回的类继续可用。

calls: int = 0


def register(cls: object) -> object:
    global calls
    calls = calls + 1
    return cls


def twice(cls: object) -> object:
    global calls
    calls = calls + 1
    return cls


@register
@twice
class Box:
    n: int = 3


print(calls)
print(Box.__name__)
print(Box().n)
print(Box)

# --- expect ---
# 2
# Box
# 3
# <class 'Box'>
