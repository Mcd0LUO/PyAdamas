# 328_type_tags_isinstance —— P2 运行期类型标签：unknown 值的 isinstance 全类型识别
# pyadamas: type-tags
#
# 门控 PYADAMAS_TYPE_TAGS=1（测试运行器按文件首行指令自动开关，见
# tests/run_tests.sh）。静态类型已知时 None/bool/int/float/str/bytes/list/
# dict/tuple/set 互为不同类型；0.339 下 bool/None 与 int 碰撞、tuple 无标签。
# expect 由 CPython 3.14 逐行对拍得到。
def row(x: object) -> None:
    print(isinstance(x, int), isinstance(x, bool), isinstance(x, float),
          isinstance(x, str), isinstance(x, bytes), isinstance(x, list),
          isinstance(x, dict), isinstance(x, tuple), isinstance(x, set),
          isinstance(x, object), repr(x))
row(1)
row(True)
row(False)
row(None)
row(2.5)
row("s")
row(b"xy")
row((1, 2))
row([1, 2])
row({"a": 1})
row({1, 2})
# bool 是 int 的子类：isinstance(True, int) 为 True（CPython 口径）
print(isinstance(True, int), isinstance(1, bool), isinstance(False, int))
# --- expect ---
# True False False False False False False False False True 1
# True True False False False False False False False True True
# True True False False False False False False False True False
# False False False False False False False False False True None
# False False True False False False False False False True 2.5
# False False False True False False False False False True 's'
# False False False False True False False False False True b'xy'
# False False False False False False False True False True (1, 2)
# False False False False False True False False False True [1, 2]
# False False False False False False True False False True {'a': 1}
# False False False False False False False False True True {1, 2}
# True False True
