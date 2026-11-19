# u3_unknown_arith —— unknown 上的动态算术/比较分派（int 与 boxed float 混跑）
N = 400000

xs: list[object] = [1, 2.5, 3, 4.5, 7]
i: int = 0
acc: object = 0
while i < N:
    acc = acc + xs[i % 5]
    if xs[i % 5] == 2.5:
        acc = acc + 0.5
    i = i + 1
print(acc)
