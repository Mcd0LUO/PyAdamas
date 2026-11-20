from dataclasses import dataclass


@dataclass
class Item:
    name: str
    qty: int = 1


def make(n: str) -> Item:
    return Item(n, 5)
