class S:
    excs: list[unknown]

    def __init__(self, *excs) -> None:
        self.excs = []
        for e in excs:
            self.excs.append(e)

    def __enter__(self) -> None:
        return None

    def __exit__(self, et, ev, tb) -> bool:
        print("et", str(et))
        for e in self.excs:
            print("is", et is e, "isinst", isinstance(ev, e))
            if et is e:
                return True
        return False

with S(ValueError):
    raise ValueError("boom")
print("after")
with S(IndexError):
    raise ValueError("nope")
print("after2")
