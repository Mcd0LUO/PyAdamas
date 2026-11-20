# 330_type_tags_pprint —— P2：stdlib/pprint.py（pformat/pprint/PrettyPrinter）
# pyadamas: type-tags
#
# pprint 是独立模块：tag 必须跨模块调用边界（P2 的 pa_ptag_in/len 侧信道）。
# 单行放得下时按 repr 口径（与 CPython 逐字节一致），放不下按 CPython 的
# 断行缩进/键值对齐口径。
# 文档化边界：不排序 dict 键（用例用已按 key 排好序的字面量）；容器元素
# 无逐元素 tag，嵌套 bool/None/tuple/set/bytes 不在本用例范围。
import pprint

pprint.pprint({"a": [1, 2, 3], "b": {"c": 4}})
pprint.pprint((1, 2, 3))
pprint.pprint({1, 2, 3})
pprint.pprint(b"xy")
pprint.pprint(True)
pprint.pprint(None)
pprint.pprint(2.5)
pprint.pprint("s")
pprint.pprint(list(range(30)))
pprint.pprint({"k0": 0, "k1": 1, "k2": 2, "k3": 3, "k4": 4,
               "k5": 5, "k6": 6, "k7": 7, "k8": 8, "k9": 9})
pprint.pprint(["aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
               "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"])
print(pprint.pformat({"a": [1, 2]}))
pp = pprint.PrettyPrinter()
pp.pprint([1, 2, 3])
# --- expect ---
# {'a': [1, 2, 3], 'b': {'c': 4}}
# (1, 2, 3)
# {1, 2, 3}
# b'xy'
# True
# None
# 2.5
# 's'
# [0,
#  1,
#  2,
#  3,
#  4,
#  5,
#  6,
#  7,
#  8,
#  9,
#  10,
#  11,
#  12,
#  13,
#  14,
#  15,
#  16,
#  17,
#  18,
#  19,
#  20,
#  21,
#  22,
#  23,
#  24,
#  25,
#  26,
#  27,
#  28,
#  29]
# {'k0': 0,
#  'k1': 1,
#  'k2': 2,
#  'k3': 3,
#  'k4': 4,
#  'k5': 5,
#  'k6': 6,
#  'k7': 7,
#  'k8': 8,
#  'k9': 9}
# ['aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',
#  'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb']
# {'a': [1, 2]}
# [1, 2, 3]
