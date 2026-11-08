# expect: 'Point' object has no attribute 'z'
class Point:
    x: int

p = Point()
print(p.z)
