# if / while / for / break / continue / else

def sign(x: int) -> str:
    if x < 0:
        return "neg"
    elif x == 0:
        return "zero"
    else:
        return "pos"

x = 0
while x < 5:
    print("x =", x)
    x += 1

for i in range(3):
    print("i =", i)

total = 0
for i in range(10):
    if i == 3:
        continue
    if i == 7:
        break
    total += i
print("total =", total)

i = 0
while i < 3:
    i += 1
else:
    print("while else", i)

for i in range(3):
    pass
else:
    print("for else", i)

print(sign(-5), sign(0), sign(5))

# --- expect ---
# x = 0
# x = 1
# x = 2
# x = 3
# x = 4
# i = 0
# i = 1
# i = 2
# total = 18
# while else 3
# for else 2
# neg zero pos
