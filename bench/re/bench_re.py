# bench/re/bench_re.py —— re 基准：多模式 × 多负载 × 多 API（模块级与 Pattern 各一组）
#
# 每行输出 "用例 ms"，由外部 A/B 驱动（/tmp/abre.py）交替收集取中位。
# 覆盖：字面量 / 字符类 / 量词 / 分组 / 交替 / 锚定 / 病态回溯；
#       match / search / fullmatch / findall / finditer / split / sub / compile。
# 负载分两类：大文本单次扫描（VM 路径）与「同模式反复调用」（编译路径）。
import re
import time

WORDS = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"]

def build_corpus(n: int) -> str:
    parts = []
    i = 0
    while i < n:
        w = WORDS[i % 8]
        parts.append(w)
        parts.append(" ")
        parts.append(str(i))
        parts.append(" x")
        parts.append(str(i % 97))
        parts.append(", ")
        i += 1
    return "".join(parts)

def build_lines(n: int) -> list[str]:
    out = []
    i = 0
    while i < n:
        w = WORDS[i % 8]
        out.append(w + " " + str(i) + " x" + str(i % 97))
        i += 1
    return out

TEXT = build_corpus(1200)          # ~20KB：字母 + 数字 + 逗号空格
LINES = build_lines(40)            # 短行，用于「反复调用」负载
A20 = "a" * 18 + "c"               # 病态回溯：2^18 量级

# ---------------- 大文本单次 API ----------------

def case_findall_class(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        n = len(re.findall(r"[0-9]+", t))
    return n

def case_findall_word(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        n = len(re.findall(r"\w+", t))
    return n

def case_findall_alt(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        n = len(re.findall(r"alpha|gamma|zeta", t))
    return n

def case_findall_literal(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        n = len(re.findall("alpha", t))
    return n

def case_search_literal_hit(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        m = re.search("theta", t)
        if m is not None:
            n = m.start()
    return n

def case_search_miss(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        m = re.search(r"zzz", t)
        if m is None:
            n += 1
    return n

def case_split_ws(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        n = len(re.split(r"\s+", t))
    return n

def case_sub_digits(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        n = len(re.sub(r"[0-9]+", "#", t))
    return n

def case_sub_group(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        n = len(re.sub(r"(alpha) ([0-9]+)", r"\2-\1", t))
    return n

def case_finditer(t: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        n = 0
        for m in re.finditer(r"[0-9]+", t):
            n += 1
    return n

# ---------------- 短行反复调用（编译路径） ----------------

# 初始化 PreP：实例方法里创建 Pattern（全局变量拿不到静态类型）
class ReHolder:
    def __init__(self, pat: str, flags: int) -> None:
        self.p: re.Pattern = re.compile(pat, flags)

    def findall(self, s: str) -> list[str]:
        return self.p.findall(s)

def case_init_findall(lines: list[str], reps: int) -> int:
    n = 0
    for r in range(reps):
        h = ReHolder(r"[0-9]+", 0)
        n += len(h.findall(lines[0]))
    return n

def case_lines_findall(lines: list[str], reps: int) -> int:
    n = 0
    for r in range(reps):
        for line in lines:
            n += len(re.findall(r"[0-9]+", line))
    return n

def case_lines_match(lines: list[str], reps: int) -> int:
    n = 0
    for r in range(reps):
        for line in lines:
            m = re.match(r"(\w+)\s+([0-9]+)", line)
            if m is not None:
                n += len(m.group(1)) + len(m.group(2))
    return n

def case_lines_search(lines: list[str], reps: int) -> int:
    n = 0
    for r in range(reps):
        for line in lines:
            m = re.search(r"x([0-9]+)", line)
            if m is not None:
                n += len(m.group(1))
    return n

def case_lines_split(lines: list[str], reps: int) -> int:
    n = 0
    for r in range(reps):
        for line in lines:
            n += len(re.split(r"\s+", line))
    return n

def case_lines_sub(lines: list[str], reps: int) -> int:
    n = 0
    for r in range(reps):
        for line in lines:
            n += len(re.sub(r"[0-9]", "#", line))
    return n

def case_lines_fullmatch(lines: list[str], reps: int) -> int:
    n = 0
    for r in range(reps):
        for line in lines:
            m = re.fullmatch(r"[a-z]+", "alpha")
            if m is not None:
                n += 1
    return n

# 对照：编译后的 Pattern（无重复编译开销）
def case_lines_match_pat(lines: list[str], reps: int) -> int:
    p = re.compile(r"(\w+)\s+([0-9]+)")
    n = 0
    for r in range(reps):
        for line in lines:
            m = p.match(line)
            if m is not None:
                n += len(m.group(1)) + len(m.group(2))
    return n

def case_lines_split_pat(lines: list[str], reps: int) -> int:
    p = re.compile(r"\s+")
    n = 0
    for r in range(reps):
        for line in lines:
            n += len(p.split(line))
    return n

def case_lines_sub_pat(lines: list[str], reps: int) -> int:
    p = re.compile(r"[0-9]")
    n = 0
    for r in range(reps):
        for line in lines:
            n += len(p.sub("#", line))
    return n

def case_lines_findall_pat(lines: list[str], reps: int) -> int:
    p = re.compile(r"[0-9]+")
    n = 0
    for r in range(reps):
        for line in lines:
            n += len(p.findall(line))
    return n

def case_compile_only(reps: int, _unused: int) -> int:
    n = 0
    for r in range(reps):
        p = re.compile(r"(\w+)\s+([0-9]+)")
        n += p.groups
    return n

# ---------------- 病态回溯 ----------------

def case_patho(s: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        m = re.match(r"(a+)+b", s)
        if m is None:
            n += 1
    return n

def case_patho_alt(s: str, reps: int) -> int:
    n = 0
    for r in range(reps):
        m = re.match(r"(a|aa)+$", s)
        if m is None:
            n += 1
    return n

def run(name: str, fn, a, b) -> None:
    t0 = time.perf_counter()
    n = fn(a, b)
    t1 = time.perf_counter()
    ms = (t1 - t0) * 1000.0
    print(name, ms, n)

print("re_bench text", len(TEXT), "lines", len(LINES))

run("findall_class", case_findall_class, TEXT, 120)
run("findall_word", case_findall_word, TEXT, 120)
run("findall_alt", case_findall_alt, TEXT, 120)
run("findall_literal", case_findall_literal, TEXT, 120)
run("search_literal_hit", case_search_literal_hit, TEXT, 200)
run("search_miss", case_search_miss, TEXT, 60)
run("split_ws", case_split_ws, TEXT, 100)
run("sub_digits", case_sub_digits, TEXT, 100)
run("sub_group", case_sub_group, TEXT, 100)
run("finditer", case_finditer, TEXT, 60)
run("lines_findall", case_lines_findall, LINES, 40)
run("lines_init_findall", case_init_findall, LINES, 40)
run("lines_match", case_lines_match, LINES, 40)
run("lines_search", case_lines_search, LINES, 40)
run("lines_split", case_lines_split, LINES, 40)
run("lines_sub", case_lines_sub, LINES, 40)
run("lines_fullmatch", case_lines_fullmatch, LINES, 40)
run("lines_findall_pat", case_lines_findall_pat, LINES, 40)
run("lines_match_pat", case_lines_match_pat, LINES, 40)
run("lines_split_pat", case_lines_split_pat, LINES, 40)
run("lines_sub_pat", case_lines_sub_pat, LINES, 40)
run("compile_only", case_compile_only, 3000, 0)
run("patho_plus", case_patho, A20, 2)
run("patho_alt", case_patho_alt, A20, 2)
