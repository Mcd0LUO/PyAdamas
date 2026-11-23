# pprint 常用 API 对拍。
import pprint
d = {"b": 2, "a": 1}
print("pf", pprint.pformat(d))
pprint.pprint(d)
e = [1, 2, 3]
print("pl", pprint.pformat(e))

# --- expect ---
# pf {'a': 1, 'b': 2}
# {'a': 1, 'b': 2}
# pl [1, 2, 3]
