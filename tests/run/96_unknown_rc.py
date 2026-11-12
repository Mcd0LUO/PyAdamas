# unknown 值的引用计数：dict/list 的 kind 2 元素、unknown 字段与全局在
# 覆盖/销毁时都要走注册表释放——既不能泄漏，也不能把整数当指针 dec。


def dict_unknown(flag: int) -> int:
    d = {}
    if flag > 0:
        d["k"] = list(range(3))
    else:
        d["k"] = "abc"
    d["n"] = 7          # 同一个 dict 里再塞整数，释放时必须跳过它
    return len(d)


def list_unknown(flag: int) -> int:
    xs = []
    if flag > 0:
        xs.append("abc")
        xs.append(list(range(3)))
    else:
        xs.append(7)
        xs.append(3.5)
    ys = xs[0:2]        # 切片拷贝按 kind 2 保留/释放
    zs = list(reversed(xs))
    xs[0] = xs[1]       # 覆盖旧值：新值补引用，旧值按注册表释放
    xs[1] = 9
    del xs[0]
    xs.append("tail")
    total = 0
    for i, v in enumerate(xs):
        total += i
    for a, b in zip(xs, ys):
        total += 0
    return len(xs) + len(ys) + len(zs) + total


class Box:
    def __init__(self) -> None:
        self.v = None

    def put(self, flag: int) -> None:
        if flag > 0:
            self.v = list(range(3))
        else:
            self.v = "xyz"

    def clear(self) -> None:
        self.v = None


g = None


def fill_global(flag: int) -> int:
    global g
    if flag > 0:
        g = [10, 20]
    else:
        g = "hello"
    return 1


i: int = 0
acc: int = 0
while i < 300:
    acc += dict_unknown(i % 2)
    acc += list_unknown(i % 2)
    b = Box()
    b.put(i % 2)
    b.put((i + 1) % 2)  # 覆盖 unknown 字段的旧值
    b.clear()
    acc += fill_global(i % 2)
    i += 1
print("unknown_rc", acc)

# --- expect ---
# unknown_rc 3000
