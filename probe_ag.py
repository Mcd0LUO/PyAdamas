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
            print("want", str(e), "is", et is e)
        return False

with S(ValueError):
    raise ValueError("boom")

with S(ValueError, KeyError):
    raise ValueError("boom2")
