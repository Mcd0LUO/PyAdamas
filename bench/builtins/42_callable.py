# 42_callable —— callable(函数)：编译期折叠
N = 1000000
def g() -> int:
    return 1


total = 0
for i in range(N):
    total += 1 if callable(g) else 0
print(total)
