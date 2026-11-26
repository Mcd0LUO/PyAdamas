# 0.648：跨模块类方法 / 静态方法调用（审计 A8-2）
import kcm

print(kcm.K.cm())
print(kcm.K.sm())
k = kcm.K()
print(k.v)
print(k.cm())

# --- expect ---
# 5
# 6
# 7
# 5
