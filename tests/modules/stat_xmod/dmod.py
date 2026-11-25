import os


def st(path: str) -> stat_result:
    return os.stat(path)
