# expect: infers float slots incompatible with override
class Base:
    def measure(self, value=1.25):
        return value + value


class Child(Base):
    def measure(self, value=2):
        return value + value


def read(obj: Base):
    return obj.measure()


read(Child())
