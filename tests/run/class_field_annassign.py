# 0.579：带标注的类体字段互引（ctor 复用类体求值结果，不再重算）
class C:
    x: int = 5
    y: int = x + 1
    z: int = x * 2

c = C()
print(c.x, c.y, c.z)

# --- expect ---
# 5 6 10
