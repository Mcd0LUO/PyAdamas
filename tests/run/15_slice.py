# 切片（读）：字符串、列表、元组；缺省/负边界/正负步长
s = "abcdef"
print(s[1:4])
print(s[:3], s[3:], s[:], s[-3:], s[:-2])
print(s[::2], s[::-1], s[4:1:-1])

l = [0, 1, 2, 3, 4, 5]
p = l[1:4]
print(len(p), p[0], p[2])
r = l[::-1]
print(len(r), r[0], r[5])
q = l[::2]
print(len(q), q[0], q[2])
n = l[-3:]
print(len(n), n[0])
print(len(l[4:1]))
print(len(l[10:]))

t = (0, 1, 2, 3, 4)
u = t[1:4]
print(len(u), u[0], u[2])
w = t[::-1]
print(w[0], w[-1])

words = ["a", "b", "c"]
part = words[1:]
print(part[0])

# --- expect ---
# bcd
# abc def abcdef def abcd
# ace fedcba edc
# 3 1 3
# 6 5 0
# 3 0 4
# 3 3
# 0
# 0
# 3 1 3
# 4 0
# b
