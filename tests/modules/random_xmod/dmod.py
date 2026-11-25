import random


def make(seed: int) -> random.Random:
    return random.Random(seed)
