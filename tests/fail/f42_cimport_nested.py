# expect: only allowed at module top level
def f() -> None:
    cimport clib


f()
