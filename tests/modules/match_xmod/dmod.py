import re


def find(s: str) -> re.Match:
    m = re.search("b(a+)", s)
    return m
