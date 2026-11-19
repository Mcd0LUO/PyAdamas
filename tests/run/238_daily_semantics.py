# 238_daily_semantics —— 0.316：日常语料的四个静默语义差（N2/N3/N4/N5）+ try 里 return 的 RC
#
# 0.315 及以前：
#   * N2  文本模式写文件忽略 newline=（`newline=""` / "\r\n" / "\r" 全不翻译）；
#         根因是 pa_file_open 的实参被按 4 个寄存器发射（newline/closefd 没进
#         XMM/GPR），运行时读到垃圾 → newline 恒 0。csv.writer 默认 CRLF 因此写 LF。
#   * N3  getrandbits 用 random()*2^k 而不是 CPython 的 32 位字组装；
#         randrange/sample 用 random()*n 而不是 _randbelow 拒绝采样。
#   * N4  浮点的 ',' 分组与 '#' 交替形式被静默忽略。
#   * N5  strftime 没算 tm_wday/tm_yday（%a/%A/%j/%w/%u/%U/%W 全错），
#         naive datetime 的 %z/%Z 输出本地时区。
#   * 附带：try 里 return 一个刚拼出来的值，清理先把该值释放，caller 一 dec
#         就 panic（"refcount decrement on a freed object"）—— 现在先接管
#         返回值再跑清理。
# 本用例对拍 CPython，O0/O1/O2 全一致。

import os

# ── 1) 写侧换行翻译（N2）────────────────────────────────────────
p1: str = "/tmp/pa238_a.txt"
p2: str = "/tmp/pa238_b.txt"
p3: str = "/tmp/pa238_c.txt"
p4: str = "/tmp/pa238_d.txt"
with open(p1, "w", newline="") as f:
    f.write("a\r\nb\r\n")
with open(p2, "w", newline="\r\n") as f:
    f.write("c\nd")
with open(p3, "w", newline="\r") as f:
    f.write("g\nh")
with open(p4, "w") as f:
    f.write("e\r\nf")


def raw(path: str) -> str:
    with open(path, "r", newline="") as f:
        return f.read()


print("nl empty:", repr(raw(p1)))
print("nl crlf:", repr(raw(p2)))
print("nl cr:", repr(raw(p3)))
print("nl none:", repr(raw(p4)))
# 通用换行读：\r\n / \r 都折成 \n
with open(p1, "r") as f:
    print("read universal:", repr(f.read()))
with open(p1, "r", newline="\n") as f:
    print("read nl-lf:", repr(f.read()))

# csv.writer 默认 lineterminator="\r\n"
import csv
p5: str = "/tmp/pa238.csv"
with open(p5, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["a", "1"])
    w.writerow(["b", "2"])
print("csv default:", repr(raw(p5)))
with open(p5, "w", newline="") as f:
    w2 = csv.writer(f, lineterminator="\n")
    w2.writerow(["c", "3"])
print("csv lf:", repr(raw(p5)))

# ── 2) random 序列（N3）─────────────────────────────────────────
import random
random.seed(7)
print("bits:", random.getrandbits(8), random.getrandbits(16), random.getrandbits(31),
      random.getrandbits(32), random.getrandbits(63))
random.seed(7)
print("randrange:", random.randrange(10), random.randrange(5, 100),
      random.randrange(100, 0, -7), random.randint(-3, 3))
random.seed(7)
print("sample4:", random.sample([10, 20, 30, 40], 2))
random.seed(7)
print("sample30:", random.sample(list(range(30)), 7))
random.seed(7)
print("sample-dense:", random.sample(list(range(10)), 9))
random.seed(7)
print("choices:", random.choices([10, 20, 30, 40], k=4))
random.seed(7)
xs: list[int] = list(range(12))
random.shuffle(xs)
print("shuffle:", xs)
random.seed(7)
print("choice:", random.choice(xs), random.uniform(1.0, 2.0), random.random())

# ── 3) 格式说明符（N4）─────────────────────────────────────────
print("fmt comma f:", format(1234.5678, ",.2f"), format(12345678.9, ",.0f"),
      format(-1234.5678, "_.2f"))
print("fstr comma f:", f"{1234.5678:,.2f}", f"{1e20:,.2f}")
print("fmt comma int:", format(1234567, ","), format(-1234567, ",d"))
print("fmt hash:", format(255, "#x"), format(255, "#X"), format(255, "#o"),
      format(255, "#b"), format(0, "#x"))
print("fstr hash:", f"{255:#x}", f"{255:#o}", f"{255:#b}")
print("fmt hash pad:", format(255, "#08x"), format(-255, "#08x"), format(255, "# 10x")
      if False else format(255, "#_x"))
print("fmt alt float:", format(1.0, "#.0f"), format(1e20, "#.3g"), format(0.0, "#"),
      format(12345678.9, "#.3g"))

# ── 4) strftime（N5）──────────────────────────────────────────
import datetime
dt = datetime.datetime(2026, 11, 19, 14, 30, 5)
print("strftime:", dt.strftime("%A"), dt.strftime("%a"), dt.strftime("%j"),
      dt.strftime("%w"), dt.strftime("%u"), dt.strftime("%U"), dt.strftime("%W"))
print("strftime tz:", repr(dt.strftime("%Z")), repr(dt.strftime("%z")))
print("strftime num:", dt.strftime("%Y-%m-%d %H:%M:%S"), dt.strftime("%b"), dt.strftime("%B"),
      dt.strftime("%p"), dt.strftime("%I"), dt.strftime("%%"))
for d2 in [datetime.datetime(1970, 1, 1), datetime.datetime(2000, 2, 29),
           datetime.datetime(2024, 12, 31), datetime.datetime(2020, 1, 1)]:
    print("strftime edge:", d2.isoformat(), d2.strftime("%A %j %w %U %W %u"))
print("strftime date:", datetime.date(2026, 11, 19).strftime("%A %j %w"))

# ── 5) try 里 return（清理顺序）────────────────────────────────
def pick(s: str) -> str:
    try:
        return s + "!"
    except ValueError:
        return "x"


def pick2(n: int) -> str:
    try:
        if n > 0:
            return str(n) + "-big"
        return "small"
    except ValueError:
        return "err"


def pick3(items: list[str]) -> str:
    try:
        return items[0]
    except IndexError:
        return "<empty>"


print("try return:", pick("a"), pick2(3), pick2(-1), pick3(["q"]), pick3([]))

for p in [p1, p2, p3, p4, p5]:
    os.remove(p)
# --- expect ---
# nl empty: 'a\r\nb\r\n'
# nl crlf: 'c\r\nd'
# nl cr: 'g\rh'
# nl none: 'e\r\nf'
# read universal: 'a\nb\n'
# read nl-lf: 'a\r\nb\r\n'
# csv default: 'a,1\r\nb,2\r\n'
# csv lf: 'c,3\n'
# bits: 82 62119 323946139 1695753998 445363681616962640
# randrange: 5 24 58 2
# sample4: [30, 10]
# sample30: [10, 4, 12, 20, 1, 2, 17]
# sample-dense: [5, 2, 6, 9, 0, 7, 4, 1, 3]
# choices: [20, 10, 30, 10]
# shuffle: [7, 11, 3, 10, 8, 4, 9, 1, 0, 6, 2, 5]
# choice: 4 1.9478653606090632 0.3948234964231735
# fmt comma f: 1,234.57 12,345,679 -1_234.57
# fstr comma f: 1,234.57 100,000,000,000,000,000,000.00
# fmt comma int: 1,234,567 -1,234,567
# fmt hash: 0xff 0XFF 0o377 0b11111111 0x0
# fstr hash: 0xff 0o377 0b11111111
# fmt hash pad: 0x0000ff -0x000ff 0xff
# fmt alt float: 1. 1.00e+20 0.0 1.23e+07
# strftime: Thursday Thu 323 4 4 46 46
# strftime tz: '' ''
# strftime num: 2026-11-19 14:30:05 Nov November PM 02 %
# strftime edge: 1970-01-01T00:00:00 Thursday 001 4 00 00 4
# strftime edge: 2000-02-29T00:00:00 Tuesday 060 2 09 09 2
# strftime edge: 2024-12-31T00:00:00 Tuesday 366 2 52 53 2
# strftime edge: 2020-01-01T00:00:00 Wednesday 001 3 00 00 3
# strftime date: Thursday 323 4
# try return: a! 3-big small q <empty>
