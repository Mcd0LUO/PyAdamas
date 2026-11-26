# 0.641：match 字面量模式的边界（裸星号 / bytes / 负 Big / 复数）
match b"ab":
    case b"ab":
        print("bytes ok")
    case _:
        print("bytes no")

match -100000000000000000000:
    case -100000000000000000000:
        print("negbig ok")
    case _:
        print("negbig no")

match 1 + 2j:
    case 1 + 2j:
        print("complex ok")
    case _:
        print("complex no")

match 1 - 2j:
    case 1 + 2j:
        print("complex wrong")
    case 1 - 2j:
        print("complex minus ok")
    case _:
        print("complex minus no")

# --- expect ---
# bytes ok
# negbig ok
# complex ok
# complex minus ok
