def erase(v: big) -> object:
    return v

def low4(v: object) -> object:
    return v & 15

def sq(v: object) -> object:
    return v ** 2

def put(d: dict[object, int], k: object, v: int) -> None:
    d[k] = v

def has(d: dict[object, int], k: object) -> bool:
    return k in d
