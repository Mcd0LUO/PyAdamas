# 0.562：.cpya 里声明的 C struct 可以构造（值语义 + C 布局）。
cimport szstruct as c

p = c.pair(3.0, 4.0)
print(p.a, p.b, sizeof(p))

q = c.pair()
print(q.a, q.b)

q.a = 1.25
print(q.a)

# --- expect ---
# 3.0 4.0 16
# 0.0 0.0
# 1.25
