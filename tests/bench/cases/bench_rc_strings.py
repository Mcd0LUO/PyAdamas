"""RC 基准：短命字符串的拼接、存储与释放。"""
total = 0
i = 0
while i < 20000:
    s = "value-" + str(i) + "-suffix"
    t = s + "!"
    total += len(t)
    i += 1
print(total)
