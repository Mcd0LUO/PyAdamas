# 0.642：with 的括号形式（多上下文管理器）
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
