# 容器方法语义回归（0.438）：list / dict / set / tuple 方法、别名与浅拷贝、集合运算、相等。
xs = [1, 2, 3]
xs.append(4)
xs.extend([5, 6])
xs.insert(0, 0)
xs.insert(-1, 99)
print("list_basic", xs)
p1 = xs.pop(0)
p2 = xs.pop(-1)
print("list_pop", p1, p2, xs)
xs.remove(3)
print("list_remove", xs.count(1), xs.count(42), xs.index(2))
ys = xs.copy()
ys.append(7)
zs = xs
zs.append(8)
print("list_alias", xs, ys)
nums = [3, 1, 2]
nums.sort()
print("list_sort", nums)
nums.reverse()
print("list_reverse", nums)
nums.clear()
print("list_clear", len(nums))
d = {"a": 1, "b": 2}
print("dict_get", d.get("a"), d.get("z"), d.get("z", 9))
print("dict_setdefault", d.setdefault("c", 3), d.setdefault("a", 99))
print("dict_pop", d.pop("a"), d.pop("nope", 7))
print("dict_keys", list(d.keys()), list(d.values()), list(d.items()))
d["z"] = 26
print("dict_order", list(d.keys()), list(d.values()))
del d["b"]
print("dict_del", list(d.keys()), len(d), "c" in d)
k, v = d.popitem()
print("dict_popitem", k, v, list(d.keys()))
e = {}
e.update({"x": 1})
e.update({"x": 2, "y": 3})
print("dict_update", list(e.keys()), list(e.values()))
f = e.copy()
f["w"] = 4
print("dict_copy", len(e), len(f))
print("dict_eq", {"p": 1, "q": 2} == {"q": 2, "p": 1}, {"p": 1} == {"p": 2})
s = set([3, 1, 2, 1])
s.add(9)
print("set_dedupe", len(s), 1 in s, 5 in s)
s.discard(9)
s.discard(42)
print("set_discard", sorted(list(s)))
removed = 0
try:
    s.remove(42)
except KeyError:
    removed = 1
print("set_remove_missing", removed)
s2 = s.copy()
s2.add(100)
print("set_copy", len(s), len(s2))
a = set([1, 2, 3])
b = set([2, 3, 4])
print("set_ops", sorted(list(a | b)), sorted(list(a & b)), sorted(list(a - b)), sorted(list(a ^ b)))
print("set_eq", set([1, 2]) == set([2, 1]), set([1]) == set([2]))
print("set_from", sorted(list(set([1, 1, 2]))), sorted(list(set("aab"))))
print("tuple_methods", (1, 2, 2, 3).count(2), (1, 2, 2, 3).index(2))
print("tuple_cmp", (1, 2) == (1, 2), (1, 2) == (1, 3))
print("nested_eq", [[1, 2]] == [[1, 2]], [[1, 2]] == [[2, 1]], {"k": [1]} == {"k": [1]})

# --- expect ---
# list_basic [0, 1, 2, 3, 4, 5, 99, 6]
# list_pop 0 6 [1, 2, 3, 4, 5, 99]
# list_remove 1 0 1
# list_alias [1, 2, 4, 5, 99, 8] [1, 2, 4, 5, 99, 7]
# list_sort [1, 2, 3]
# list_reverse [3, 2, 1]
# list_clear 0
# dict_get 1 None 9
# dict_setdefault 3 1
# dict_pop 1 7
# dict_keys ['b', 'c'] [2, 3] [('b', 2), ('c', 3)]
# dict_order ['b', 'c', 'z'] [2, 3, 26]
# dict_del ['c', 'z'] 2 True
# dict_popitem z 26 ['c']
# dict_update ['x', 'y'] [2, 3]
# dict_copy 2 3
# dict_eq True False
# set_dedupe 4 True False
# set_discard [1, 2, 3]
# set_remove_missing 1
# set_copy 3 4
# set_ops [1, 2, 3, 4] [2, 3] [1] [1, 4]
# set_eq True False
# set_from [1, 2] ['a', 'b']
# tuple_methods 2 1
# tuple_cmp True False
# nested_eq True False True
