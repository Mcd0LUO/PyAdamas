# 0.628：match 捕获只在整条模式（含 guard）通过后提交；重复捕获按类型比较

x = "orig"
v = ["a", 1]
match v:
    case [x, 2]:
        print("hit")
    case _:
        print("miss")
print("x =", x)

# guard 失败同样不提交
y = "keep"
match ["q", 9]:
    case [y, n] if n > 100:
        print("guarded hit")
    case _:
        print("guarded miss")
print("y =", y)

# 成功路径要提交
z = "before"
match ["w", 3]:
    case [z, n]:
        print("ok", z, n)
print("z =", z)

# 重复捕获：内容相同但不同对象
s1 = "ab"
s2 = "a" + "b"
match [s1, s2]:
    case [d, d]:
        print("dup matched")
    case _:
        print("dup miss")

# --- expect ---
# miss
# x = orig
# guarded miss
# y = keep
# ok w 3
# z = w
# dup matched
