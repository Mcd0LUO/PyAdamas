# expect: cannot assign to cimport attribute 'c.sqrt'
cimport clib as c

c.sqrt = 1
