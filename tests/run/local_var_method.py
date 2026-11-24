# 局部变量接住「工厂/方法返回值」后，类方法面不能丢（docs/stdlib-round2.md §7.2）。
#
# 复核结论（0.495）：链式调用与「先赋给局部变量」走同一条类型检查 + 降级
# 路径，静态类型相同；本用例把这个事实钉住——哪天局部变量的类型塌成 unknown，
# 「方法面只对链式/全局可用」的老症状就会在这里先红。


class Reader:
    def __init__(self, data: str) -> None:
        self.data: str = data

    def read(self) -> str:
        return self.data

    def size(self) -> int:
        return len(self.data)


class Archive:
    def __init__(self, data: str) -> None:
        self.data: str = data

    def open_member(self, name: str) -> Reader:
        return Reader(self.data + ":" + name)

    def peek(self, name: str) -> Reader:
        # 没有返回标注：返回类型靠函数体推断，调用点同样要拿到类方法面。
        return self.open_member(name)


def dump(a: Archive) -> None:
    r = a.open_member("a.txt")            # 局部变量接住
    print(r.read(), r.size())
    print(a.open_member("b.txt").read())  # 链式
    p = a.peek("c.txt")                   # 推断出来的返回类型
    print(p.read())
    pool: list[Reader] = [a.open_member("d.txt")]
    q = pool[0]                           # 从容器元素接住
    print(q.read())


a: Archive = Archive("zip")
dump(a)

# --- expect ---
# zip:a.txt 9
# zip:b.txt
# zip:c.txt
# zip:d.txt
