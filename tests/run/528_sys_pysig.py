# stdlib/sigs/sys.pysig 迁移回归：sys 常量 + 运行期成员
# 与 CPython 3.14 同形脚本对拍；PA 专有的 argv/executable 值用布尔判定规避。
import sys

print("maxsize", sys.maxsize)
print("argv_len", len(sys.argv))
print("argv0_str", isinstance(sys.argv[0], str))
print("argv_idx", isinstance(sys.argv[0], str))
print("stdin", sys.stdin is not None)
print("stdout", sys.stdout is not None)
print("stderr", sys.stderr is not None)
print("exec", isinstance(sys.executable, str))

# exc_info 在 except 块里返回三元组
try:
    raise ValueError("boom")
except ValueError as e:
    info = sys.exc_info()
    print("exc0", info[0].__name__)
    print("exc1", isinstance(info[1], object))

# from import
from sys import maxsize, argv
print("from_max", maxsize == sys.maxsize)
print("from_argv", len(argv) == len(sys.argv))

# --- expect ---
# maxsize 9223372036854775807
# argv_len 1
# argv0_str True
# argv_idx True
# stdin True
# stdout True
# stderr True
# exec True
# exc0 ValueError
# exc1 True
# from_max True
# from_argv True
