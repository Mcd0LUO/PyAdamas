# 0.537：同一块内 del 之后再读 → 报错。
y: int = 5
del y
print(y)
