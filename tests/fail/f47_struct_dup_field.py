# expect: duplicate field 'x' in struct 'P'
struct P:
    x: int
    x: float
