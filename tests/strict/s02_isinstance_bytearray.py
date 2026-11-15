# expect: does not support type 'bytearray'
# bytearray/memoryview 在 pyadamas 里没有独立表示，frozenset 被当成 set。
# 以前把它们映射到内部等价类型，导致 isinstance([1,2,3], bytearray) 返回
# True —— 为一个不存在的类型作假证。现在明确报不支持。
x = [1, 2, 3]
print(isinstance(x, bytearray))
