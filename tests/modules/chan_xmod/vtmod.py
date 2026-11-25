def producer(ch: Chan[int]) -> None:
    ch.send(42)

def plain(x: int) -> int:
    return x * 2
