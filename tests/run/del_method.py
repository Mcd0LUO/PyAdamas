# __del__（0.539）：RC 归零、dtor 之前调用一次；异常打一行到 stderr 并吞掉；
# GC 环回收不调用（与 CPython 的差异，见 docs/language.md）。


class Plain:
    def __init__(self, x: int) -> None:
        self.x = x

    def __del__(self) -> None:
        print("plain del", self.x)


class Boom:
    def __init__(self) -> None:
        self.v = 7

    def __del__(self) -> None:
        raise ValueError("boom")


class Node:
    def __init__(self) -> None:
        self.peer = None

    def __del__(self) -> None:
        print("node del")


a: Plain = Plain(1)
print("before")

b: Boom = Boom()
print("after boom")

n1: Node = Node()
n2: Node = Node()
n1.peer = n2
n2.peer = n1
print("cycle ok")

# --- expect ---
# before
# after boom
# cycle ok
# plain del 1
# __del__ raised ValueError: boom
