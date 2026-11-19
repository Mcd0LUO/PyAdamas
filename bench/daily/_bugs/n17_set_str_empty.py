# NEW BUG N17 (wrong + crash): annotated empty set[str] = set() does not honor
# the str element type for .add() of DYNAMIC (distinct-object, equal-content)
# strings: no dedup (pointer/machine-word compare) + RC panic at exit (exit 1).
# CPython: 1 ['xy']. pyadamas: 2 ['xy','xy'] + "refcount decrement on a freed object".
# Interned literals coincidentally dedup (same pointer), masking the bug.
# set(list) and set-comprehension DO propagate the element type (workarounds);
# dict[str, int] empty-annotated also works. Suspect: empty set() lowering passes
# unknown key-kind to the runtime table unlike the dict annotation path.
s: set[str] = set()
a: str = "x" + "y"
b: str = "x" + "y"
s.add(a)
s.add(b)
print("set dyn concat:", len(s), sorted(s))
