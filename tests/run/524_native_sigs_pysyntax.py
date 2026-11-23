# 524_native_sigs_pysyntax.py —— 声明式签名的完整覆盖面（.pysig 是 Python 语法）
#
# P0 第二轮：stdlib/sigs/*.pysig 从自定义语法换成 Python 语法（Lexer/Parser +
# 注解复用 typecheck）。签名本身的行为必须与迁移前一致：这个用例把 time 的
# 8 个函数 + 2 个常量、os.path 的 16 个函数**全部**调用一遍，并与 CPython 对拍。
#
# 覆盖到的声明机制：
#   * 约定推导运行期符号（pa_time_sleep / pa_os_path_join）
#   * 带默认值的可选参数（localtime/gmtime 的 -1e300 哨兵 = 取当前时间）
#   * 可变参（strftime 的 *args: unknown、join 的 *parts: str）
#   * tuple[int, ...] / tuple[str, str] 返回类型
#   * 带注解的常量（CLOCK_REALTIME / CLOCK_MONOTONIC）
#   * from-import 的原生名解析（native_fn 走声明表）

import os.path as osp
import time
from os.path import (abspath, basename, dirname, exists, expanduser, getsize,
                     getmtime, isdir, isfile, islink, join, normpath, realpath,
                     samefile, split, splitext)
from time import (gmtime, localtime, mktime, monotonic, perf_counter, sleep,
                  strftime, time)

# ── time：8 个函数 + 2 个常量 ────────────────────────────────────────
print("time_positive", time() > 0)
t0 = monotonic()
sleep(0.005)
print("monotonic_advances", monotonic() >= t0)
print("perf_positive", perf_counter() > 0)
print("gmtime_epoch", gmtime(0)[:6] == (1970, 1, 1, 0, 0, 0))
print("localtime_len", len(localtime(0)) == 9)
print("strftime", strftime("%Y-%m-%d", (2000, 1, 2, 3, 4, 5, 0, 0, 0)))
print("mktime_positive", mktime((2000, 1, 1, 0, 0, 0, 0, 0, 0)) > 0)
print("clock_consts", time.CLOCK_REALTIME, time.CLOCK_MONOTONIC)

# ── os.path：16 个函数 ──────────────────────────────────────────────
print("join", join("a", "b", "c"))
print("basename", basename("/x/y/z.txt"))
print("dirname", dirname("/x/y/z.txt"))
print("splitext", splitext("a/b.tar.gz"))
print("split", split("/x/y/z.txt"))
print("normpath", normpath("/a//b/../c"))
print("abspath_abs", abspath("/tmp") == "/tmp")
print("expanduser_home", expanduser("~")[0] == "/")
print("realpath_tmp", realpath("/tmp") == "/tmp")
print("exists", exists("/tmp"), isdir("/tmp"), isfile("/etc/hostname"))
print("missing", exists("/nonexistent-pyadamas-xyz"), islink("/nonexistent-pyadamas-xyz"))
print("getsize_positive", getsize("/etc/hostname") > 0)
print("getmtime_positive", getmtime("/etc/hostname") > 0)
print("samefile", samefile("/tmp", "/tmp"), samefile("/tmp", "/etc"))
print("osp_alias", osp.basename("/q/w"), osp.isdir("/tmp"))

# --- expect ---
# time_positive True
# monotonic_advances True
# perf_positive True
# gmtime_epoch True
# localtime_len True
# strftime 2000-01-02
# mktime_positive True
# clock_consts 0 1
# join a/b/c
# basename z.txt
# dirname /x/y
# splitext ('a/b.tar', '.gz')
# split ('/x/y', 'z.txt')
# normpath /a/c
# abspath_abs True
# expanduser_home True
# realpath_tmp True
# exists True True True
# missing False False
# getsize_positive True
# getmtime_positive True
# samefile True False
# osp_alias w True
