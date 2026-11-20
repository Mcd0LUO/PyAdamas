# 329_type_tags_repr —— P2：unknown 值的 repr 按运行期类型码分派
# pyadamas: type-tags
#
# 0.339 的 repr(unknown) 只能靠堆探针：None/bool 打成 0/1、tuple 打成指针、
# set 打成内部 dict、bytes 打成 'xy'。类型标签后按类型码分派。
# 容器元素没有逐元素 tag（文档化边界）：嵌套值仍按探针口径（int/float/
# str/list/dict 正确；嵌套 bool/None/tuple/set/bytes 不在本用例范围）。
def show(x: object) -> None:
    print(repr(x))
show(None)
show(True)
show(False)
show(0)
show(-3)
show(2.5)
show("a'b")
show(b"a\x00b\t'\\")
show(b"xy")
show(())
show((1,))
show((1, 2))
show(set())
show({1, 2})
show({"a"})
show([1, "a", 2.5])
show({"k": [1, 2]})
show(("a", 1))
show({1: "x", 2: "y"})
# --- expect ---
# None
# True
# False
# 0
# -3
# 2.5
# "a'b"
# b"a\x00b\t'\\"
# b'xy'
# ()
# (1,)
# (1, 2)
# set()
# {1, 2}
# {'a'}
# [1, 'a', 2.5]
# {'k': [1, 2]}
# ('a', 1)
# {1: 'x', 2: 'y'}
