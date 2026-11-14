# isinstance/bytes 前端与运行期：元组第二参数、bytes/type/frozenset、bytes() 构造。
b = b'hi'
s = 'hi'
l = [1, 2, 3]
t = (1, 2)
d = {'a': 1}
print(isinstance(b, bytes), isinstance(s, str), isinstance(b, str))
print(isinstance(b, (str, bytes)), isinstance(s, (str, bytes)), isinstance(123, (str, bytes)))
print(isinstance(l, (list, tuple)), isinstance(t, (list, tuple)))
print(isinstance(d, dict), isinstance(1.5, (int, float)), isinstance(3, int))

class A: pass
class B(A): pass
a = A()
bb = B()
print(isinstance(a, (A, B)), isinstance(bb, A), isinstance(bb, B))

print(bytes(3) == b'\x00\x00\x00')
print(bytes((72, 105)) == b'Hi')

# --- expect ---
# True True False
# True True False
# True True
# True True True
# True True True
# True
# True
