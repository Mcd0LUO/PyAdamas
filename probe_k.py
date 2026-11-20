class CM:
    def __enter__(self) -> int:
        return 1

    def __exit__(self, et, ev, tb) -> bool:
        print("exit args", str(et), str(ev), str(tb))
        return True

with CM():
    raise ValueError("boom")
print("after")
