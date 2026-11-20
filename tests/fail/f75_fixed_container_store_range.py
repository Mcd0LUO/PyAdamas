# expect: literal 200 is out of range for 'i8'
l: list[i8] = [1]
l[0] = 200
