def show(e: unknown) -> None:
    print("direct", str(e))

def var(*excs) -> None:
    for e in excs:
        print("var", str(e))

show(ValueError)
var(ValueError)
var(ValueError, KeyError)

class H:
    def __init__(self, e: unknown) -> None:
        self.e = e
    def get(self) -> str:
        return str(self.e)

print("field", H(ValueError).get())
