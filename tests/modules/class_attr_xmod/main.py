import kattr

# 类属性：类对象上读
print(kattr.C.LIMIT)
print(kattr.C.NAME)

# 实例上读（沿 MRO 找到声明者那一份）
c = kattr.C()
print(c.LIMIT, c.NAME, c.get_limit())

# from-import 的类同样能读
from kattr import C
print(C.LIMIT)

# 继承来的类属性
print(kattr.Base.X, kattr.Sub.X)
print(kattr.Sub().X)

# 定义模块里改过之后，导入方读到的是同一份存储（Python 的类属性是共享的）
c.bump()
print(kattr.C.LIMIT)

# 带标注的不算类属性：仍是 A8-1 的拒绝口径，这里只验证类属性本身
# --- expect ---
# 42
# hi
# 42 hi 42
# 42
# 7 7
# 7
# 43
