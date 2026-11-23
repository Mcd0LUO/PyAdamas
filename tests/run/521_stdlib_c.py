# stdlib batch C regression: itertools / functools / logging
# 同形脚本与 CPython 3.14 对拍，输出逐行一致（差异项见 docs/stdlib-gaps.md 第 9 节）。
# codecs / pathlib 需模块表登记，本轮不在此文件（见报告「待补」）。
import itertools
import functools
import logging
import os

# ── itertools ──
print("it_starmap", itertools.starmap(lambda a,b: a+b, [(1,2),(3,4)]))
print("it_starmap3", itertools.starmap(lambda a,b,c: a*b+c, [[1,2,3],[4,5,6]]))
print("it_tee", itertools.tee([1,2,3], 2))
print("it_tee3", itertools.tee(["a","b"], 3))
print("it_chain", itertools.chain_iterables([[1],[2,3],[4]]))

# starmap 的 arity 分派边界（1/4 参数）
print("it_s1", itertools.starmap(lambda a: a*2, [[1],[2],[3]]))
print("it_s4", itertools.starmap(lambda a,b,c,d: a+b+c+d, [[1,2,3,4],[5,6,7,8]]))

# ── functools.cmp_to_key ──
key = functools.cmp_to_key(lambda a,b: a-b)
print("ft_asc", sorted([3,1,2], key=key))
key = functools.cmp_to_key(lambda a,b: b-a)
print("ft_desc", sorted([3,1,2], key=key))
key = functools.cmp_to_key(lambda a,b: a-b)
print("ft_dup", sorted([5,2,5,1,2], key=key))

def cmp_len(a: str, b: str) -> int:
    return len(a) - len(b)
key = functools.cmp_to_key(cmp_len)
print("ft_strlen", sorted(["bbb", "a", "cc"], key=key))

# ── logging ──
log_path = "/tmp/pyadamas_stdlib_c_521.log"
logger = logging.getLogger("batchC")
logger.setLevel(logging.DEBUG)
fmt = logging.Formatter("%(levelname)s|%(name)s|%(message)s")
fh = logging.FileHandler(log_path)
fh.setFormatter(fmt)
logger.addHandler(fh)
logger.debug("dbg")
logger.info("inf")
logger.warning("wrn")
logger.error("err")
fh.close()

# 读回文件内容，把「文件内容」纳入逐行对拍
saved = open(log_path, "r").read()
print("lg_content", repr(saved))
os.remove(log_path)
print("lg_removed", 1)

# StreamHandler 构造 + setFormatter + addHandler（emit 写 stderr，会污染
# 测试捕获，这里只验证对象可创建与装配，实际 emit 在 docs 对拍里验证）
sh = logging.StreamHandler()
sh.setFormatter(logging.Formatter("[%(levelname)s] %(name)s: %(message)s"))
logger2 = logging.getLogger("streamC")
logger2.setLevel(logging.INFO)
logger2.addHandler(sh)
print("lg_stream", 1)

# --- expect ---
# it_starmap [3, 7]
# it_starmap3 [5, 26]
# it_tee [[1, 2, 3], [1, 2, 3]]
# it_tee3 [['a', 'b'], ['a', 'b'], ['a', 'b']]
# it_chain [1, 2, 3, 4]
# it_s1 [2, 4, 6]
# it_s4 [10, 26]
# ft_asc [1, 2, 3]
# ft_desc [3, 2, 1]
# ft_dup [1, 2, 2, 5, 5]
# ft_strlen ['a', 'cc', 'bbb']
# lg_content 'DEBUG|batchC|dbg\nINFO|batchC|inf\nWARNING|batchC|wrn\nERROR|batchC|err\n'
# lg_removed 1
# lg_stream 1
