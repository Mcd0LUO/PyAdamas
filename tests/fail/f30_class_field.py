# expect: field 'x' has type 'int', cannot assign 'str'
class Point:
    x: int

p = Point()
p.x = "s"
