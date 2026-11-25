# expect: bound method's receiver 'xs' was reassigned after binding
# B1 健全性（0.535）：绑定点不在调用点重放接收者——接收者被重新赋值后调用必须拒绝，
# 不能静默改成对新对象的调用（CPython 调用的是原对象的方法）。
xs: list[int] = [1]
ys: list[int] = [2]
push = xs.append
xs = ys
push(3)
print(xs, ys)
