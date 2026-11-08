# expect: unexpected keyword argument 'c'
def h(a: int) -> int:
    return a

h(1, c=2)
