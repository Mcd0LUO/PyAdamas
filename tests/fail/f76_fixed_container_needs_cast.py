# expect: list element type is 'i8', got 'int'
l: list[i8] = []
n: int = 5
l.append(n)
