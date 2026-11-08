# expect: list element type is 'int', got 'str'
xs: list[int] = [1, 2]
xs.append("s")
