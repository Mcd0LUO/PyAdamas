# NEW BUG N7 (silent wrong): filter(None, heterogeneous_list) keeps the empty
# string (truthiness on unknown str wrong) and prints unknown elements raw
# (no repr quotes). CPython: [1, 'a', 2]. pyadamas: [1, , a, 2].
# Homogeneous filter(None, list[str]/list[int]) is correct.
# Suspect: filter(None) desugar truthiness uses integer/len path for unknown;
# unknown-element list print lacks repr. Workaround: explicit comprehension
# [x for x in xs if x] on a typed list, or filter with a typed predicate.
xs: list[object] = [0, 1, "", "a", 2]
print("filter hetero:", list(filter(None, xs)))
ys: list[str] = ["", "a", "", "b"]
print("filter homo str:", list(filter(None, ys)))   # matches ['a','b']
