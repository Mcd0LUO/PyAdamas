# stdlib 审计续集回归（0.444）：re / collections / itertools / functools / statistics / pathlib / hashlib。
import re
import collections
import itertools
import functools
import statistics
import pathlib
import hashlib

print("findall_plain", re.findall("[a-z]+", "ab cd ef"))
print("findall_group1", re.findall("([a-z]+)", "ab cd"))
print("findall_group2", re.findall("(a)(b)", "ab ab"))
print("sub", re.sub("[0-9]+", "#", "a1b22c333"))
print("sub_count", re.sub("[0-9]+", "#", "a1b22c333", count=2))
print("split", re.split(",", "a,b,c"))
print("split_capture", re.split("(,)", "a,b,c"))
print("escape", re.escape("a.b*c"))
print("flags", re.findall("abc", "ABC abc", re.I), re.findall("^a", "a\nab", re.M))
m1 = re.search("world", "hello world")
if m1 is not None:
    print("span", m1.start(), m1.end(), m1.span())
m2 = re.compile("(?P<w>[a-z]+)").search("xy z")
if m2 is not None:
    print("named", m2.group(0), m2.group("w"))
print("quant", re.findall("a{2,3}", "a aa aaa aaaa"), re.findall("<.+?>", "<a><b>"))
dq = collections.deque([1, 2, 3])
dq.append(4)
dq.appendleft(0)
print("deque", list(dq), len(dq))
print("deque_pop", dq.pop(), dq.popleft())
dq.extend([5, 6])
dq.extendleft([9, 8])
print("deque_extend", list(dq))
dq.rotate(2)
print("rotate", list(dq))
print("index_count", dq.index(3), dq.count(2))
dq.reverse()
print("reverse", list(dq))
c = collections.Counter(["a", "b", "a", "c", "a"])
print("counter", c["a"], c.most_common(2))
od = collections.OrderedDict()
od["x"] = 1
od["y"] = 2
print("ordered", list(od.keys()))
dd = collections.defaultdict(int)
dd["a"] = dd["a"] + 2
dd["b"] = dd["b"] + 3
print("defaultdict", dd["a"], dd["b"], dd["missing"])
print("chain", list(itertools.chain([1, 2], [3, 4])))
print("repeat", list(itertools.repeat("x", 3)), list(itertools.repeat(7, 0)))
print("islice", list(itertools.islice([0, 1, 2, 3, 4], 3)))
print("product", list(itertools.product([1, 2], ["a", "b"])))
print("perm", list(itertools.permutations([1, 2, 3], 2)))
print("comb", list(itertools.combinations([1, 2, 3], 2)))
print("comb_wr", list(itertools.combinations_with_replacement([1, 2], 2)))
print("accumulate", list(itertools.accumulate([1, 2, 3, 4])))
zl = list(itertools.zip_longest([1, 2], [3]))
print("zip_longest", len(zl), zl[0][0], zl[1][0])
def add(a: int, b: int) -> int:
    return a + b


inc = functools.partial(add, 1)
print("partial", inc(5))
print("reduce", functools.reduce(add, [1, 2, 3, 4]), functools.reduce(add, [1, 2], 10))
def fadd(x: float, y: float) -> float:
    return x + y


print("reduce_float", functools.reduce(fadd, [1.5, 2.5]))
calls = [0]
@functools.lru_cache
def square(x: int) -> int:
    calls[0] = calls[0] + 1
    return x * x


print("cache", square(3), square(3), square(4), calls[0])
data = [1.0, 2.0, 3.0, 4.0]
print("stats", statistics.mean(data), statistics.fmean(data), statistics.median(data))
print("stats2", statistics.median_low(data), statistics.median_high(data), statistics.variance(data))
print("stdev", statistics.stdev([2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0]))
print("harm_geom", statistics.harmonic_mean([1.0, 2.0, 4.0]), statistics.geometric_mean([1.0, 4.0]))
print("mode", statistics.mode([1, 2, 2, 3]))
p = pathlib.Path("a/b/c.txt")
print("path", str(p), p.name, p.stem, p.suffix, str(p.parent))
print("path_join", str(p / "d"), p.parts[0], p.is_absolute())
print("hash", hashlib.md5(b"abc").hexdigest(), hashlib.sha1(b"abc").hexdigest())
print("hash256", hashlib.sha256(b"abc").hexdigest())

# --- expect ---
# findall_plain ['ab', 'cd', 'ef']
# findall_group1 ['ab', 'cd']
# findall_group2 [('a', 'b'), ('a', 'b')]
# sub a#b#c#
# sub_count a#b#c333
# split ['a', 'b', 'c']
# split_capture ['a', ',', 'b', ',', 'c']
# escape a\.b\*c
# flags ['ABC', 'abc'] ['a', 'a']
# span 6 11 (6, 11)
# named xy xy
# quant ['aa', 'aaa', 'aaa'] ['<a>', '<b>']
# deque [0, 1, 2, 3, 4] 5
# deque_pop 4 0
# deque_extend [8, 9, 1, 2, 3, 5, 6]
# rotate [5, 6, 8, 9, 1, 2, 3]
# index_count 6 1
# reverse [3, 2, 1, 9, 8, 6, 5]
# counter 3 [('a', 3), ('b', 1)]
# ordered ['x', 'y']
# defaultdict 2 3 0
# chain [1, 2, 3, 4]
# repeat ['x', 'x', 'x'] []
# islice [0, 1, 2]
# product [(1, 'a'), (1, 'b'), (2, 'a'), (2, 'b')]
# perm [(1, 2), (1, 3), (2, 1), (2, 3), (3, 1), (3, 2)]
# comb [(1, 2), (1, 3), (2, 3)]
# comb_wr [(1, 1), (1, 2), (2, 2)]
# accumulate [1, 3, 6, 10]
# zip_longest 2 1 2
# partial 6
# reduce 10 13
# reduce_float 4.0
# cache 9 9 16 2
# stats 2.5 2.5 2.5
# stats2 2.0 3.0 1.6666666666666667
# stdev 2.138089935299395
# harm_geom 1.7142857142857142 2.0
# mode 2
# path a/b/c.txt c.txt c .txt a/b
# path_join a/b/c.txt/d a False
# hash 900150983cd24fb0d6963f7d28e17f72 a9993e364706816aba3e25717850c26c9cd0d89d
# hash256 ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
