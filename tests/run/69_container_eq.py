# 容器逐元素相等（CPython 语义，不再比指针）
a = [1, 2]
b = [1, 2]
print(a == b)
print(a != b)
print([1, 2] == [1, 3])
print(["a", "b"] == ["a", "b"])
print([1.5] == [1.5])
print([1, 2] == (1, 2))
print((1, 2) == (1, 2))
print((1, 2) != (1, 3))
d = {"k": 1}
print(d == {"k": 1})
print(d == {"k": 2})
print({"a": 1, "b": 2} == {"b": 2, "a": 1})
s = {1, 2}
print(s == {1, 2})
print(s == {1, 3})
print(s != {1, 3})
x = [1, 2]
print(x == x)

# 嵌套容器：同构的能逐层比下去
print([[1]] == [[1]])
print([[1]] == [[2]])
print([["a"], ["b"]] == [["a"], ["b"]])
print({"k": [1]} == {"k": [1]})
print({"k": [1]} == {"k": [2]})
print([[1], [2]] == [[1], [2]])
print([[[1]]] == [[[1]]])

# dict/set 打印真实内容
print({"a": 1, "b": 2})
print({1, 2, 3})
print({"k": [1, 2]})
print({"k": "v"})
print({})
# --- expect ---
# True
# False
# False
# True
# True
# False
# True
# True
# True
# False
# True
# True
# False
# True
# True
# True
# False
# True
# True
# False
# True
# True
# {'a': 1, 'b': 2}
# {1, 2, 3}
# {'k': [1, 2]}
# {'k': 'v'}
# {}
