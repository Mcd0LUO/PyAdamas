# expect: argument 'x' of 'char_id' expects 'i8', got 'int'
cimport cfixrange as f
n: int = 5
f.char_id(n)
