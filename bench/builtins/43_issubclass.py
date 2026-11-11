# 43_issubclass —— issubclass(类, 类)：编译期折叠
N = 1000000
class A:
    pass


class B(A):
    pass


total = 0
for i in range(N):
    total += 1 if issubclass(B, A) else 0
print(total)
