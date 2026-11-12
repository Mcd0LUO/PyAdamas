base: int = 10
tag: bytes = b"pre"

def add(a: int, b: int) -> int:
    return a + b

def wrap(x: bytes) -> bytes:
    return x + b"!"

def scale(x: float, k: float = 2.0) -> float:
    return x * k
