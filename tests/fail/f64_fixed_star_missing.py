# expect: missing required argument: 'value'
def identity(value):
    return value


identity(*())
