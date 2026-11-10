def show(p):
    print(p)


def pair(a, b):
    print(a, b)


def kw(s, n):
    print(s, n)


def inner(t):
    print(t)


def outer():
    inner("nested")


def poly(v):
    print("poly-called")


class Box:
    def __init__(self, v):
        self.v = v

    def show(self, p):
        print("box", p)

    def tag(self, s):
        print("tag", s)


show("abc")
show("def")
pair("x", 1)
kw(s="k", n=7)
outer()
poly("s")
poly(1)
b = Box(3)
b.show("hello")
b.tag("world")
print(b.v)

items = ["a", "b"]
for it in items:
    show(it)

# --- expect ---
# abc
# def
# x 1
# k 7
# nested
# poly-called
# poly-called
# box hello
# tag world
# 3
# a
# b
