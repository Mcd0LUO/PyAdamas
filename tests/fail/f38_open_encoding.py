# expect: open() encoding 'utf-16' is not supported
def read_utf16() -> None:
    f = open("/tmp/py23_utf16.txt", encoding="utf-16")
    f.close()
