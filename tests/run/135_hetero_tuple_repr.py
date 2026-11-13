# 异构元组的 repr/str：元素按各自静态类型渲染，嵌套元组递归处理。

class P:
    def __repr__(self) -> str:
        return "P()"


t = (1, "a", 2.5, True, None)
print(t)
print(repr(t))
print(str(t))
print(repr((1,)))
print(repr(()))
print(repr((1, 2.5)))
print(repr(("x", (1, 2.5))))
print(repr((P(), 1)))

# --- expect ---
# (1, 'a', 2.5, True, None)
# (1, 'a', 2.5, True, None)
# (1, 'a', 2.5, True, None)
# (1,)
# ()
# (1, 2.5)
# ('x', (1, 2.5))
# (P(), 1)
