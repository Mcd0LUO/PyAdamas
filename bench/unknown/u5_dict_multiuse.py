d: dict[str, object] = {"a": 1.5, "b": 2.5}
acc: object = 0
i: int = 0
while i < 400000:
    v: object = d["a"]
    acc = acc + v
    if v == 1.5:
        acc = acc + 0.25
    i = i + 1
print(acc)
