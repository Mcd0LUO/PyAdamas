# copy 浅/深拷贝对拍；copy()/deepcopy() 本轮修了分派 bug。
import copy
a = [1, [2, 3]]
b = copy.copy(a)
print("c", b, b is a, b[1] is a[1])
d = {"x": 1}
e = copy.copy(d)
print("cd", e, e is d)
c = copy.deepcopy(a)
print("deep", c, c is a, c[1] is a[1])
dd = copy.deepcopy(d)
print("deepd", dd, dd is d)

# --- expect ---
# c [1, [2, 3]] False True
# cd {'x': 1} False
# deep [1, [2, 3]] False False
# deepd {'x': 1} False
