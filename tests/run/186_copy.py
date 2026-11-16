# copy 模块（stdlib/copy.py）：按容器类型分开的显式入口。
#
# 语义边界：PyAdamas 是静态类型编译器，isinstance 不能把 unknown 形参收窄成
# 具体容器类型，所以没法写成「一个函数处理所有容器」；这里提供 copy_list /
# copy_dict / deepcopy_list / deepcopy_dict 这类显式入口。
#
# 注意：嵌套容器的 `==` 在本项目里按元素指针比较（docs/compatibility.md 的已知
# 差异），所以下面用「内层长度 + is 身份」验证深拷贝，不用 ==。
import copy

xs: list = [1, 2, 3]
ys: list = copy.copy_list(xs)
ys.append(4)
print(len(xs))
print(len(ys))

d: dict = {"a": 1}
e: dict = copy.copy_dict(d)
e["b"] = 2
print(len(d))
print(len(e))

# 深拷贝：内层容器是新的对象（is 为 False），但内容长度一致
nested: list = [[1, 2], [3]]
deep: list = copy.deepcopy_list(nested)
print(len(nested))
print(len(deep))
print(len(nested[0]))
print(len(deep[0]))
print(nested[0] is deep[0])

# 浅拷贝：内层共享（is 为 True）
shallow: list = copy.copy_list(nested)
print(shallow[0] is nested[0])

# --- expect ---
# 3
# 4
# 1
# 2
# 2
# 2
# 2
# 2
# False
# True
