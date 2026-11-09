xs = [3, 1, 2]
xs.extend([5, 4])
print(len(xs))
xs.insert(0, 9)
print(xs[0], len(xs))
xs.remove(1)
print(len(xs))
print(xs.pop(), xs.pop(0), len(xs))
xs.clear()
print(len(xs))
ys = [1, 2, 1]
print(ys.index(2), ys.count(1))
zs = ys.copy()
zs.append(3)
print(len(ys), len(zs))
rs = [1, 2, 3]
rs.reverse()
print(rs[0])
ss = [3, 1, 2]
ss.sort()
print(ss[0], ss[2])
d = {1: 10}
print(d.setdefault(1, 99), d.setdefault(2, 20), len(d))
e = d.copy()
e[3] = 30
print(len(d), len(e))
t = (1, 2, 2)
print(t.count(2), t.index(2))
print((5).bit_length(), (0).bit_length(), (255).bit_length())

# --- expect ---
# 5
# 9 6
# 5
# 4 9 3
# 0
# 1 2
# 3 4
# 3
# 1 3
# 10 20 2
# 2 3
# 2 1
# 3 0 8
