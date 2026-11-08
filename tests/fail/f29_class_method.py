# expect: 'Point' object has no method 'draw'
class Point:
    x: int

p = Point()
p.draw()
