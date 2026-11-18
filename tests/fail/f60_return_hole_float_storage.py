# expect: return of 'make' has incompatible float storage
def make() -> tuple[float, list]:
    return (1, [1.25])


make()
