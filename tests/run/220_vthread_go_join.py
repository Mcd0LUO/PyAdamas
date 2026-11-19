# 虚拟线程先验：go/join 基础、多参/str 参、嵌套 go、返回值丢弃
def add(a, b):
    print("add", a + b)

def greet(name):
    print("hello", name)

h1 = go add(3, 4)
h2 = go greet("vt")
join(h1)
join(h2)
print("joined")

def child(x):
    print("child", x)

def parent():
    hc = go child(9)
    join(hc)
    print("parent done")

hp = go parent()
join(hp)

def ret():
    return "unused"

go ret()
print("end")
# --- expect ---
# add 7
# hello vt
# joined
# child 9
# parent done
# end
