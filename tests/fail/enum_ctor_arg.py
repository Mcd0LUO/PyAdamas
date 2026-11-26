# expect: enum 'Color' expects an int value, got 'str'
# P1：给 int 值枚举传 str 以前能编过、运行期段错误（coerce 静默放过 str，比较指令
# 按 str 语义生成 pa_str_eq）。现在在 typecheck 层拒绝（本模块与跨模块共用
# TypeChecker::check_enum_call）。
from enum import Enum


class Color(Enum):
    RED = 1
    GREEN = 2


print(Color('RED'))
