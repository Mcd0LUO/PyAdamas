# 529_pysig_merge.py —— .pysig 与硬编码**按名字合并**（声明优先、未声明保留）
#
# 背景：pysig hook 以前是整模块替换，sys 里无法用声明表达的 6 个运行期成员
# （argv/executable/标准流/exc_info）会直接消失。现在默认按名字合并：
# 声明里的 func/const 覆盖硬编码；声明里没有的成员保留硬编码签名与降级映射。
# 本用例同时覆盖两类成员与 from-import。

import sys

# 声明侧（stdlib/sigs/sys.pysig）：maxsize 常量 + exit 函数
print("maxsize", sys.maxsize)
# 硬编码侧（builtin_sigs.cpp 的 sys 基础分支）
print("argv_len", len(sys.argv) >= 1)
print("executable_nonempty", sys.executable != "")
print("stdout_ok", sys.stdout is not None)
print("exc_info_none", sys.exc_info()[0] is None)

from sys import maxsize as m2
from sys import argv as av
print("from_import_declared", m2 == sys.maxsize)
print("from_import_hardcoded", len(av) == len(sys.argv))
# --- expect ---
# maxsize 9223372036854775807
# argv_len True
# executable_nonempty True
# stdout_ok True
# exc_info_none True
# from_import_declared True
# from_import_hardcoded True
