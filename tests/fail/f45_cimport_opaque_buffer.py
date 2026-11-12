# expect: expects an opaque C pointer
cimport cstdio

cstdio.fputs("x", [1.0])
