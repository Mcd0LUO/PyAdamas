import io


def make(s: str) -> io.StringIO:
    b: io.StringIO = io.StringIO()
    b.write(s)
    b.seek(0)
    return b
