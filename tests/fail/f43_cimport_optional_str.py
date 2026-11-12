# expect: may be None
cimport clib

print(clib.strlen(clib.getenv("PATH")))
