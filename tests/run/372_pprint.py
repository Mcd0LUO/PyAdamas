# 372_pprint —— pprint 与 CPython 对拍（sort_dicts 默认排序 + 参数面）
# pyadamas: type-tags
#
# 覆盖面：pformat/pprint/PrettyPrinter；sort_dicts=True 默认按键排序（含嵌套
# dict、混类型键、单行放得下的情形）；sort_dicts=False 保持插入序；
# indent（0/2/4）、width（1/5/10/20/40/80）、depth（1/2/3）、compact；
# 长字符串按空白折行；空容器；ValueError 口径（width=0/indent<0/depth<=0）。
# 期望输出由 CPython 3.14.4 pprint 逐行生成（见文件尾）。
# 文档化边界（P2 容器元素无逐元素 tag）：嵌套 bool/None/tuple/set/bytes 与
# 同构 float 容器会退化，用例不含这些形状。
import pprint

D = {"b": 1, "a": [1, 2, 3], "c": {"z": 0, "y": []}}
pprint.pprint(D)
print(pprint.pformat(D))
print(pprint.pformat(D, sort_dicts=False))
print(pprint.pformat(D, sort_dicts=False, width=20))
print(pprint.pformat({"mixed": 1, 2: "two"}))
print(pprint.pformat({"b": 2, "a": 1, "c": 3}))
print(pprint.pformat({"beta": [1, 2, 3], "alpha": [4, 5, 6],
                      "gamma": {"z": 9, "y": 8}}, width=30))
print(pprint.pformat([{"b": 1, "a": 2}, {"d": 3, "c": 4}]))

# width / indent
pprint.pprint(list(range(12)))
pprint.pprint({"a": 1, "b": 2}, indent=2, width=10)
print(pprint.pformat([[1, 2], [3, 4]], indent=2))
print(pprint.pformat([1, [2, [3, 4]]], width=10))
print(pprint.pformat({"a": {"b": {"c": {"d": 1}}}}, width=15))
print(pprint.pformat([[[1, 2], [3, 4]], [[5, 6], [7, 8]]], indent=4, width=30))

# depth
print(pprint.pformat({"a": [1, 2], "b": {"c": 3}}, depth=1))
print(pprint.pformat({"a": [1, 2], "b": {"c": 3}}, depth=2))
print(pprint.pformat([1, [2, [3, [4]]]], depth=3))

# compact
print(pprint.pformat([1, [2, [3, [4, 5]]]], compact=True, width=20))
print(pprint.pformat(["aaaa", "bbbb", "cccc", "dddd"], compact=True, width=15))
print(pprint.pformat([1, 2, 3, 4, 5, 6], compact=True, width=20))
print(pprint.pformat([1, 2, 3, 4, 5, 6], width=20))

# 容器形状
print(pprint.pformat({}))
print(pprint.pformat([]))
print(pprint.pformat(()))
print(pprint.pformat((1,)))
print(pprint.pformat((1, 2)))
print(pprint.pformat({1, 2, 3}))
print(pprint.pformat("word word word word word word word word", width=20))
print(pprint.pformat("word word word word word word word word", width=30))
print(pprint.pformat(chr(10).join(["line one", "line two with longer text"]),
                      width=20))
print(pprint.pformat({"long": "aaa bbb ccc ddd eee fff ggg hhh"}, width=20))

# PrettyPrinter
pp = pprint.PrettyPrinter(indent=2, width=20)
print(pp.pformat([["a", "b"], ["c", "d"]]))
print(pp.pformat({"b": 1, "a": 2}))
pp2 = pprint.PrettyPrinter()
print(pp2.pformat({"b": 1, "a": 2}))
pp3 = pprint.PrettyPrinter(width=10, compact=True)
print(pp3.pformat([1, [2, [3, [4, 5]]]]))
pprint.pprint({"k0": 0, "k1": 1, "k2": 2}, indent=2)

# ValueError 口径
try:
    pprint.pformat([], width=0)
except ValueError as e:
    print("ValueError:", e)
try:
    pprint.pformat([], indent=-1)
except ValueError as e:
    print("ValueError:", e)
try:
    pprint.pformat([], depth=0)
except ValueError as e:
    print("ValueError:", e)

# PrettyPrinter 构造期校验（CPython 在 __init__ 里报，不等 pformat）
try:
    pprint.PrettyPrinter(width=0)
except ValueError as e:
    print("ValueError:", e)
try:
    pprint.PrettyPrinter(indent=-1)
except ValueError as e:
    print("ValueError:", e)
try:
    pprint.PrettyPrinter(depth=0)
except ValueError as e:
    print("ValueError:", e)
# --- expect ---
# {'a': [1, 2, 3], 'b': 1, 'c': {'y': [], 'z': 0}}
# {'a': [1, 2, 3], 'b': 1, 'c': {'y': [], 'z': 0}}
# {'b': 1, 'a': [1, 2, 3], 'c': {'z': 0, 'y': []}}
# {'b': 1,
#  'a': [1, 2, 3],
#  'c': {'z': 0,
#        'y': []}}
# {2: 'two', 'mixed': 1}
# {'a': 1, 'b': 2, 'c': 3}
# {'alpha': [4, 5, 6],
#  'beta': [1, 2, 3],
#  'gamma': {'y': 8, 'z': 9}}
# [{'a': 2, 'b': 1}, {'c': 4, 'd': 3}]
# [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11]
# { 'a': 1,
#   'b': 2}
# [[1, 2], [3, 4]]
# [1,
#  [2,
#   [3, 4]]]
# {'a': {'b': {'c': {'d': 1}}}}
# [   [[1, 2], [3, 4]],
#     [[5, 6], [7, 8]]]
# {'a': [...], 'b': {...}}
# {'a': [1, 2], 'b': {'c': 3}}
# [1, [2, [3, [...]]]]
# [1,
#  [2, [3, [4, 5]]]]
# ['aaaa',
#  'bbbb',
#  'cccc',
#  'dddd']
# [1, 2, 3, 4, 5, 6]
# [1, 2, 3, 4, 5, 6]
# {}
# []
# ()
# (1,)
# (1, 2)
# {1, 2, 3}
# ('word word word '
#  'word word word '
#  'word word')
# ('word word word word word '
#  'word word word')
# ('line one\n'
#  'line two with '
#  'longer text')
# {'long': 'aaa bbb '
#          'ccc ddd '
#          'eee fff '
#          'ggg hhh'}
# [ ['a', 'b'],
#   ['c', 'd']]
# {'a': 2, 'b': 1}
# {'a': 2, 'b': 1}
# [1,
#  [2,
#   [3,
#    [4,
#     5]]]]
# {'k0': 0, 'k1': 1, 'k2': 2}
# ValueError: width must be != 0
# ValueError: indent must be >= 0
# ValueError: depth must be > 0
# ValueError: width must be != 0
# ValueError: indent must be >= 0
# ValueError: depth must be > 0
