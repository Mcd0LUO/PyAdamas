# 控制流
total = 0
for i in range(10):
    if i % 2 == 0:
        continue
    total += i
    if total > 20:
        break

i = 0
while i < 5:
    i += 1
else:
    print("loop finished")

result = "big" if total > 10 else "small"

if total > 100:
    grade = "A"
elif total > 50:
    grade = "B"
elif total > 20:
    grade = "C"
else:
    grade = "D"

for i, ch in enumerate("abc"):
    print(i, ch)

for k, v in {"a": 1, "b": 2}.items():
    print(k, v)

# 单行 suite
if total: print(total)
while i > 0: i -= 1
