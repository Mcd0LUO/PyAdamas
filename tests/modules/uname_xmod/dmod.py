import os


def info() -> uname_result:
    return os.uname()
