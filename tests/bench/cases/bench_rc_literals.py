"""RC 基准：含新值的 list / tuple / dict 字面量构造。"""
total = 0
i = 0
while i < 10000:
    xs = ["a" + str(i), "b" + str(i), "c" + str(i)]
    t = ("x" + str(i), "y" + str(i))
    d = {"k" + str(i): "v" + str(i)}
    total += len(xs) + len(t) + len(d)
    i += 1
print(total)
