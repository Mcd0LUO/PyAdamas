# expect: got multiple values for argument 'second'
def pair(first, second):
    return first + second


pair(*(1, 2), second=3)
