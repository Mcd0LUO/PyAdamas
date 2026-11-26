# BUG-3：无公共基类的异构类实例放进容器，print 输出裸指针（元素类型降级成
# unknown，运行期 repr 探测认不出实例）。修复后按元素真实类调 __repr__/__str__。
class A:
    def __repr__(self) -> str:
        return 'A()'

class B:
    def __repr__(self) -> str:
        return 'B()'

class StrOnly:
    def __str__(self) -> str:
        return 'StrOnly-str'

class Plain:
    pass

print([A(), A()])
print([A(), B()])
print((A(), B()))

d = {'x': A(), 'y': B()}
print(d)

nested = [[A(), B()], [B(), A()]]
print(nested)

# 裸 print 走 str 语义（先 __str__ 再 __repr__）
print(StrOnly())

# 混入标量时同一容器逐元素分派
print([1, A()])
print([A(), 'x'])

# 没有 __str__/__repr__ 的类：打出 <X object>（与 09_class.py 的
# class_default_str 同形；CPython 是 <X object at 0x...>，本仓库文档化的缺省
# repr 形状差异）。至少不再是裸指针。
print([A(), Plain()])

# --- expect ---
# [A(), A()]
# [A(), B()]
# (A(), B())
# {'x': A(), 'y': B()}
# [[A(), B()], [B(), A()]]
# StrOnly-str
# [1, A()]
# [A(), 'x']
# [A(), <Plain object>]
