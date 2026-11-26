class K:
    v: int = 0

    def __init__(self) -> None:
        self.v = 7

    @classmethod
    def cm(cls) -> int:
        return 5

    @staticmethod
    def sm() -> int:
        return 6
