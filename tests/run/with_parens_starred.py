# 0.642：with 的括号形式 / 星号赋值目标 / 裸 except 位置
class C:
    def __enter__(self):
        print("enter")
        return self

    def __exit__(self, a, b, c):
        print("exit")
        return False


with (C(), C()):
    print("body")

with (C() as x, C() as y):
    print("as body")

a, *b = [1, 2, 3]
print(a, b)

# --- expect ---
# enter
# enter
# body
# exit
# exit
# enter
# enter
# as body
# exit
# exit
# 1 [2, 3]
