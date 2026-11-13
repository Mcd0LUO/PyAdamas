"""json 基准：dumps 小对象（dict[str, int]）。"""
import json

obj = {"n": 42, "m": 43, "k": 7}

total = 0
for _ in range(20000):
    total += len(json.dumps(obj))
print(total)
