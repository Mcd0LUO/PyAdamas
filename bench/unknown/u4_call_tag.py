def addu(a: object, b: object) -> object:
    return a + b
acc: object = 0
i: int = 0
while i < 400000:
    acc = addu(acc, 0.5)
    i = i + 1
print(acc)
