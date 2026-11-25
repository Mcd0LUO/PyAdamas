# 0.536：buffer 越界必须 panic（不静默）。
b = buffer(2)
b[0] = 1.0
print(b[2])
