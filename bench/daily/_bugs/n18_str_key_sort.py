# NEW BUG N18 (crash): sorted(xs, key=f) where f returns a heap str over-releases
# RC at scale (exit 1, "refcount decrement on a freed object"); output is correct.
# CPython: 2000 sorted strings, rc 0. pyadamas: same stdout but rc 1.
# int/float-returning keys and natural sorted(xs) are fine. Small n may not trip.
# Suspect: key-decoration (Schwartzian) temp pairs mishandle heap-key RC.
# Workaround: int/float key, composite int key, or natural sort for str.
rows: list[str] = []
i: int = 0
while i < 2000:
    rows.append("s" + str((i * 37) % 2000))
    i += 1
def idk(s: str) -> str:
    return s
ordered: list[str] = sorted(rows, key=idk)
print("scalar str str-key:", len(ordered), ordered[0])
