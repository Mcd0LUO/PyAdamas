# unsupported: class body statement
class Config:
    debug = True
    names = ["a", "b"]

    def describe(self) -> str:
        return "config"
