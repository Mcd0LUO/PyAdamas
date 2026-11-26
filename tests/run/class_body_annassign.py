# 0.578：类体求值表 —— 带标注的类体赋值可以被后面的类体语句读到
class C:
    x: int = 5
    y = x + 1
    z = x * 2

c = C()
print(c.x, c.y, c.z)

# 嵌套类同名不串帧（419 的回归防护）
def outer() -> int:
    c: int = 3
    class K:
        c: str = "y"
    return c + len(K().c)

print(outer())

# --- expect ---
# 5 6 10
# 4
