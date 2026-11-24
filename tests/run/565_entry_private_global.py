# 入口模块私有标量全局跨调用提升：语义对拍用例
def twice(x: int) -> int:
    return x * 2

# 1) 私有全局 + 调用：可提升，结果必须是完整累加
acc = 0
for i in range(10):
    acc = acc + twice(i)
print(acc)

# 2) 全局被别的函数读：不得提升（bump 必须看到循环中的最新值）
g = 0
def peek() -> int:
    return g
for i in range(3):
    g = g + peek() + 1
print(g)

# 3) 方法调用不碰全局：可提升
class C:
    def val(self) -> int:
        return 3
c = C()
sum3 = 0
for i in range(5):
    sum3 = sum3 + c.val()
print(sum3)

# 4) 浮点私有全局
facc = 0.0
def quarter() -> float:
    return 0.25
for i in range(4):
    facc = facc + quarter()
print(facc)

# 5) 循环变量本身的出口值（CPython：range 循环后是最后一个元素）
r = -1
for r in range(4):
    pass
print(r)

# 6) while + 私有全局 + 调用
w = 0
j = 0
while j < 5:
    w = w + twice(1)
    j = j + 1
print(w, j)

# --- expect ---
# 90
# 7
# 15
# 1.0
# 3
# 10 5
