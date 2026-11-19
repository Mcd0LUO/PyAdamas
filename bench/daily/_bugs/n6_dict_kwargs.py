# NEW BUG N6 (silent wrong): dict(**kwargs) constructor yields a statically
# unknown dict -> prints '<dict object>' instead of contents.
# CPython: {'x': 1, 'y': 2}. pyadamas: <dict object>.
# Suspect: kwargs ctor path does not infer dict[str, V] static type.
# Workaround: dict literal {'x': 1, 'y': 2} or dict(pairs_list).
print("dict kwargs:", dict(x=1, y=2))
print("dict literal:", {"x": 1, "y": 2})   # matches
