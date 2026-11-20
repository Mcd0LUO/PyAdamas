"""difflib —— 序列比对与文本 diff（纯 pyadamas 源码，stdlib3）。

覆盖 SequenceMatcher（find_longest_match / get_matching_blocks /
get_opcodes / get_grouped_opcodes / ratio / quick_ratio /
real_quick_ratio / set_seqs / set_seq1 / set_seq2）、get_close_matches、
unified_diff 与 Match。

语义边界（轻量标尺，见 docs/stdlib-batch3.md）：
- SequenceMatcher 的 a/b 先转成统一序列（str 按码点拆、list 原样）；
  元素按值比较/哈希（pa 的 dict 键语义），junk 函数（isjunk 非 None）会
  通过 unknown 函数值调用；autojunk 的 popular 元素剔除同 CPython；
- unified_diff / get_grouped_opcodes 返回 list（CPython 是生成器）；
  unified_diff 的内容行只加前缀、不补 lineterm（同 CPython），lineterm 仅用于
  文件头与 @@ 行；
- get_close_matches 收 list[str]，内部用 SequenceMatcher 比值；
- 不建模 IS_LINE_JUNK / IS_CHARACTER_JUNK 与 ndiff / context_diff。
"""


def _to_list(x: unknown) -> list[unknown]:
    return list(x)


def _eq(x: unknown, y: unknown) -> bool:
    return x == y


def _call1(f: unknown, x: unknown) -> unknown:
    return f(x)


def _indices(d: dict[unknown, list[int]], k: unknown) -> list[int]:
    if k in d:
        return d[k]
    return []


class Match:
    """一段匹配：a[i:a.size] == b[j:j+size]。"""

    a: int
    b: int
    size: int

    def __init__(self, a: int, b: int, size: int) -> None:
        self.a = a
        self.b = b
        self.size = size

    def __len__(self) -> int:
        return 3

    def __getitem__(self, i: int) -> int:
        if i == 0:
            return self.a
        if i == 1:
            return self.b
        if i == 2:
            return self.size
        raise IndexError("tuple index out of range")

    def __repr__(self) -> str:
        return ("Match(a=" + str(self.a) + ", b=" + str(self.b) +
                ", size=" + str(self.size) + ")")


class SequenceMatcher:
    """两个序列的最长匹配块/编辑操作。"""

    isjunk: unknown
    a: list[unknown]
    b: list[unknown]
    autojunk: bool
    b2j: dict[unknown, list[int]]
    bjunk: list[unknown]
    bpopular: list[unknown]
    matching_blocks: list[Match]
    opcodes: list[tuple[str, int, int, int, int]]

    def __init__(self, isjunk: unknown = None, a: unknown = "",
                 b: unknown = "", autojunk: bool = True) -> None:
        self.isjunk = isjunk
        self.a = []
        self.b = []
        self.autojunk = autojunk
        self.b2j = {}
        self.bjunk = []
        self.bpopular = []
        self.matching_blocks = []
        self.opcodes = []
        self.set_seqs(a, b)

    def _is_junk(self, elt: unknown) -> bool:
        if self.isjunk is None:
            return False
        f = self.isjunk
        r = _call1(f, elt)
        if r is None:
            return False
        if isinstance(r, bool):
            return r
        return r != 0

    def set_seqs(self, a: unknown, b: unknown) -> None:
        self.set_seq1(a)
        self.set_seq2(b)

    def set_seq1(self, a: unknown) -> None:
        self.a = _to_list(a)
        self.matching_blocks = []
        self.opcodes = []

    def set_seq2(self, b: unknown) -> None:
        self.b = _to_list(b)
        self.matching_blocks = []
        self.opcodes = []
        self._chain_b()

    def _chain_b(self) -> None:
        b = self.b
        n = len(b)
        self.b2j = {}
        self.bjunk = []
        self.bpopular = []
        i = 0
        while i < n:
            elt = b[i]
            if self._is_junk(elt):
                if elt not in self.bjunk:
                    self.bjunk.append(elt)
                i += 1
                continue
            if elt in self.b2j:
                self.b2j[elt].append(i)
            else:
                self.b2j[elt] = [i]
            i += 1
        if not self.autojunk or n < 200:
            return
        ntest = n // 100 + 1
        kept: dict[unknown, list[int]] = {}
        for elt in self.b2j:
            lst = self.b2j[elt]
            if len(lst) > ntest:
                self.bpopular.append(elt)
            else:
                kept[elt] = lst
        self.b2j = kept

    def find_longest_match(self, alo: int, ahi: int, blo: int,
                           bhi: int) -> Match:
        a = self.a
        b = self.b
        besti = alo
        bestj = blo
        bestsize = 0
        j2len: dict[int, int] = {}
        i = alo
        while i < ahi:
            newj2len: dict[int, int] = {}
            for j in _indices(self.b2j, a[i]):
                if j < blo:
                    continue
                if j >= bhi:
                    break
                if j - 1 in j2len:
                    k = j2len[j - 1] + 1
                else:
                    k = 1
                newj2len[j] = k
                if k > bestsize:
                    besti = i - k + 1
                    bestj = j - k + 1
                    bestsize = k
            j2len = newj2len
            i += 1
        # 先向两侧扩「非 junk」，再扩「两侧相同的 junk」（同 CPython：只查
        # b 侧是不是 junk）
        while besti > alo and bestj > blo:
            if self._is_junk(b[bestj - 1]):
                break
            if not _eq(a[besti - 1], b[bestj - 1]):
                break
            besti = besti - 1
            bestj = bestj - 1
            bestsize = bestsize + 1
        while besti + bestsize < ahi and bestj + bestsize < bhi:
            if self._is_junk(b[bestj + bestsize]):
                break
            if not _eq(a[besti + bestsize], b[bestj + bestsize]):
                break
            bestsize = bestsize + 1
        while besti > alo and bestj > blo:
            if not self._is_junk(b[bestj - 1]):
                break
            if not _eq(a[besti - 1], b[bestj - 1]):
                break
            besti = besti - 1
            bestj = bestj - 1
            bestsize = bestsize + 1
        while besti + bestsize < ahi and bestj + bestsize < bhi:
            if not self._is_junk(b[bestj + bestsize]):
                break
            if not _eq(a[besti + bestsize], b[bestj + bestsize]):
                break
            bestsize = bestsize + 1
        return Match(besti, bestj, bestsize)

    def get_matching_blocks(self) -> list[Match]:
        if len(self.matching_blocks) > 0:
            return self.matching_blocks
        queue: list[tuple[int, int, int, int]] = []
        queue.append((0, len(self.a), 0, len(self.b)))
        found: list[Match] = []
        while len(queue) > 0:
            q = queue.pop()
            alo = q[0]
            ahi = q[1]
            blo = q[2]
            bhi = q[3]
            m = self.find_longest_match(alo, ahi, blo, bhi)
            if m.size == 0:
                continue
            found.append(m)
            if alo < m.a and blo < m.b:
                queue.append((alo, m.a, blo, m.b))
            if m.a + m.size < ahi and m.b + m.size < bhi:
                queue.append((m.a + m.size, ahi, m.b + m.size, bhi))
        found = _sort_matches(found)
        found.append(Match(len(self.a), len(self.b), 0))
        self.matching_blocks = found
        return self.matching_blocks

    def get_opcodes(self) -> list[tuple[str, int, int, int, int]]:
        if len(self.opcodes) > 0:
            return self.opcodes
        out: list[tuple[str, int, int, int, int]] = []
        i = 0
        j = 0
        for m in self.get_matching_blocks():
            tag = ""
            if i < m.a and j < m.b:
                tag = "replace"
            elif i < m.a:
                tag = "delete"
            elif j < m.b:
                tag = "insert"
            if tag != "":
                out.append((tag, i, m.a, j, m.b))
            if m.size > 0:
                out.append(("equal", m.a, m.a + m.size, m.b, m.b + m.size))
            i = m.a + m.size
            j = m.b + m.size
        self.opcodes = out
        return self.opcodes

    def get_grouped_opcodes(self, n: int = 3) -> list[list[tuple[str, int, int, int, int]]]:
        codes = self.get_opcodes()
        if len(codes) == 0:
            codes = [("equal", 0, 1, 0, 1)]
        first = codes[0]
        if first[0] == "equal":
            i1 = first[1]
            i2 = first[2]
            j1 = first[3]
            j2 = first[4]
            start_i = i2 - n
            if i1 > start_i:
                start_i = i1
            start_j = j2 - n
            if j1 > start_j:
                start_j = j1
            codes[0] = ("equal", start_i, i2, start_j, j2)
        last = codes[len(codes) - 1]
        if last[0] == "equal":
            i1 = last[1]
            i2 = last[2]
            j1 = last[3]
            j2 = last[4]
            end_i = i1 + n
            if i2 < end_i:
                end_i = i2
            end_j = j1 + n
            if j2 < end_j:
                end_j = j2
            codes[len(codes) - 1] = ("equal", i1, end_i, j1, end_j)
        nn = n + n
        groups: list[list[tuple[str, int, int, int, int]]] = []
        group: list[tuple[str, int, int, int, int]] = []
        for code in codes:
            tag = code[0]
            i1 = code[1]
            i2 = code[2]
            j1 = code[3]
            j2 = code[4]
            if tag == "equal" and i2 - i1 > nn:
                stop_i = i1 + n
                if i2 < stop_i:
                    stop_i = i2
                stop_j = j1 + n
                if j2 < stop_j:
                    stop_j = j2
                group.append((tag, i1, stop_i, j1, stop_j))
                groups.append(group)
                group = []
                i1 = i2 - n
                if i1 < 0:
                    i1 = 0
                if code[1] > i1:
                    i1 = code[1]
                j1 = j2 - n
                if j1 < 0:
                    j1 = 0
                if code[3] > j1:
                    j1 = code[3]
            group.append((tag, i1, i2, j1, j2))
        if len(group) > 0:
            is_only_equal = len(group) == 1 and group[0][0] == "equal"
            if not is_only_equal:
                groups.append(group)
        return groups

    def ratio(self) -> float:
        matches = 0
        for m in self.get_matching_blocks():
            matches = matches + m.size
        total = len(self.a) + len(self.b)
        if total == 0:
            return 1.0
        return 2.0 * matches / total

    def quick_ratio(self) -> float:
        total = len(self.a) + len(self.b)
        if total == 0:
            return 1.0
        avail: dict[unknown, int] = {}
        for elt in self.b2j:
            avail[elt] = len(self.b2j[elt])
        matches = 0
        for elt in self.a:
            if elt in avail:
                n = avail[elt]
                if n > 0:
                    matches = matches + 1
                    avail[elt] = n - 1
        return 2.0 * matches / total

    def real_quick_ratio(self) -> float:
        la = len(self.a)
        lb = len(self.b)
        if la + lb == 0:
            return 1.0
        if la < lb:
            return 2.0 * la / (la + lb)
        return 2.0 * lb / (la + lb)


def _sort_matches(xs: list[Match]) -> list[Match]:
    out: list[Match] = []
    for m in xs:
        pos = len(out)
        i = 0
        while i < len(out):
            o = out[i]
            if o.a > m.a or (o.a == m.a and o.b > m.b):
                pos = i
                break
            i += 1
        out.insert(pos, m)
    return out


def _ratio_pair(a: unknown, b: unknown) -> float:
    sm = SequenceMatcher(None, a, b)
    return sm.ratio()


def get_close_matches(word: str, possibilities: list[str], n: int = 3,
                      cutoff: float = 0.6) -> list[str]:
    """在 possibilities 里挑与 word 最像的 n 个（比值 >= cutoff）。"""
    if n <= 0:
        return []
    best_r: list[float] = []
    best_i: list[int] = []
    i = 0
    while i < len(possibilities):
        r = _ratio_pair(word, possibilities[i])
        if r >= cutoff:
            # 同 CPython 的 (ratio, index) 堆口径：同分时 index 大的排前面
            pos = -1
            k = 0
            while k < len(best_r):
                if r > best_r[k]:
                    pos = k
                    break
                if r == best_r[k] and i > best_i[k]:
                    pos = k
                    break
                k += 1
            if pos < 0 and len(best_r) < n:
                pos = len(best_r)
            if pos >= 0:
                best_r.insert(pos, r)
                best_i.insert(pos, i)
                if len(best_r) > n:
                    best_r.pop()
                    best_i.pop()
        i += 1
    out: list[str] = []
    for idx in best_i:
        out.append(possibilities[idx])
    return out


def _format_range_unified(start: int, stop: int) -> str:
    begin = start + 1
    length = stop - start
    if length == 1:
        return str(begin)
    return str(begin) + "," + str(length)


def unified_diff(a: list[str], b: list[str], fromfile: str = "",
                 tofile: str = "", fromfiledate: str = "",
                 tofiledate: str = "", n: int = 3,
                 lineterm: str = "\n") -> list[str]:
    """两个行序列的 unified diff（返回行列表）。"""
    out: list[str] = []
    out.append("--- " + fromfile + _date_suffix(fromfiledate) + lineterm)
    out.append("+++ " + tofile + _date_suffix(tofiledate) + lineterm)
    sm = SequenceMatcher(None, a, b)
    for group in sm.get_grouped_opcodes(n):
        first = group[0]
        last = group[len(group) - 1]
        r1 = _format_range_unified(first[1], last[2])
        r2 = _format_range_unified(first[3], last[4])
        out.append("@@ -" + r1 + " +" + r2 + " @@" + lineterm)
        for code in group:
            tag = code[0]
            i1 = code[1]
            i2 = code[2]
            j1 = code[3]
            j2 = code[4]
            # 内容行只加前缀，不补 lineterm（同 CPython：调用方给的 a/b
            # 行自己带换行符时输出才带换行）
            if tag == "equal":
                i = i1
                while i < i2:
                    out.append(" " + a[i])
                    i += 1
            if tag == "replace" or tag == "delete":
                i = i1
                while i < i2:
                    out.append("-" + a[i])
                    i += 1
            if tag == "replace" or tag == "insert":
                j = j1
                while j < j2:
                    out.append("+" + b[j])
                    j += 1
    return out


def _date_suffix(date: str) -> str:
    if date == "":
        return ""
    return "\t" + date
