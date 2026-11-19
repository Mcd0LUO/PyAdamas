# NEW BUG N16 (crash): dict.setdefault(k, []).append(v) -> RC panic exit 1.
# "pyadamas: runtime error: refcount decrement on a freed object".
# Chained form crashes mid-run; binding the result to a var still panics at exit.
# CPython: [1, 2]. Suspect: the fresh default [] temp is over-released when the
# key already exists (returned existing list RC not bumped). int default is fine.
# Workaround: if k not in d: d[k] = []  then d[k].append(v); or defaultdict(list).
d: dict[str, list[int]] = {}
d.setdefault("k", []).append(1)
d.setdefault("k", []).append(2)
print("setdefault chain:", d["k"])
