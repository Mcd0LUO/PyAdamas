# expect: must return int
class Bad:
    def __len__(self):
        return "two"

print(len(Bad()))
