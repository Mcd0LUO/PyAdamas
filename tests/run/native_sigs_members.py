# 原生/标准库接口表补齐：常量值 + 新补的运行期函数（输出与 CPython 3.14 一致）。
#
# 这些成员以前在 --imports / build 模式下报 module 'X' has no attribute 'Y'，
# 或者返回 unknown 让调用点的类型检查全部失效。常量按**目标平台**（Linux
# x86-64）与项目对标的 CPython 3.14.4 固定，见 stdlib/sigs/*.pysig 的注释。
import datetime
import os
import random
import re
import sys
import time

# ── 常量：值与 CPython 3.14.4 逐字一致 ──────────────────────────
print(sys.platform, sys.byteorder, sys.maxunicode)
print(sys.hexversion, sys.api_version, sys.dont_write_bytecode)
print(sys.version_info[0], sys.version_info[1], sys.version_info[3])
print(sys.version.startswith("3.14"))
print(os.name, os.curdir, os.pardir, os.altsep, os.pathsep, os.extsep, os.devnull)
# re 的标志位在 CPython 里是 IntFlag（打印成 re.ASCII），这里取 int 比数值。
print(int(re.A), int(re.ASCII), int(re.U), int(re.UNICODE), int(re.NOFLAG))
print(datetime.MINYEAR, datetime.MAXYEAR)

# ── 新补的运行期函数 ──────────────────────────────────────────
print(len(os.urandom(16)), type(os.urandom(0)).__name__)
print(os.strerror(2))
print(os.getuid() >= 0, os.geteuid() >= 0)
print(time.time_ns() > 0, time.monotonic_ns() > 0, time.perf_counter_ns() > 0)
print(time.process_time() >= 0.0)
random.seed(7)
print(random.randbytes(12))
print(sys.intern("interned") == "interned")
print(re.purge())

# --- expect ---# --- expect ---
# linux little 1114111
# 51250416 1013 False
# 3 14 final
# True
# posix . .. None : . /dev/null
# 256 256 32 32 0
# 1 9999
# 16 bytes
# No such file or directory
# True True
# True True True
# True
# b'8\xb4\xe6R\xe4M\xa7\xf27\r\x9e&'
# True

# --- expect ---
# linux little 1114111
# 51250416 1013 False
# 3 14 final
# True
# posix . .. None : . /dev/null
# 256 256 32 32 0
# 1 9999
# 16 bytes
# No such file or directory
# True True
# True True True
# True
# b'8\xb4\xe6R\xe4M\xa7\xf27\r\x9e&'
# True
# None
