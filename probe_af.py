class H:
    def close(self) -> None:
        print("closed")

class C:
    thing: unknown
    def __init__(self, thing: unknown) -> None:
        self.thing = thing
    def __enter__(self) -> unknown:
        return self.thing
    def __exit__(self, et, ev, tb) -> bool:
        x = self.thing
        x.close()
        return False

with C(H()) as v:
    print("body")
print("done")
