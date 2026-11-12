# json.dumps 的 indent= / sort_keys=（和 CPython 输出逐行一致）
import json

d = {"b": 1, "a": 2}
print(json.dumps(d))
print(json.dumps(d, indent=2))
print(json.dumps(d, sort_keys=True))
print(json.dumps(d, indent=2, sort_keys=True))

xs = [1, 2, 3]
print(json.dumps(xs))
print(json.dumps(xs, indent=2))

nested = {"k": [1, 2], "j": [3, 4]}
print(json.dumps(nested, indent=2, sort_keys=True))
# --- expect ---
# {"b": 1, "a": 2}
# {
#   "b": 1,
#   "a": 2
# }
# {"a": 2, "b": 1}
# {
#   "a": 2,
#   "b": 1
# }
# [1, 2, 3]
# [
#   1,
#   2,
#   3
# ]
# {
#   "j": [
#     3,
#     4
#   ],
#   "k": [
#     1,
#     2
#   ]
# }
