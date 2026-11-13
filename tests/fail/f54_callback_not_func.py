# expect: callback 'cmp' expects a function
cimport clib as c

c.qsort(None, 0, 8, 1)
