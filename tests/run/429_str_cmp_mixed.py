# == / != 的类型对：str/bytes 在左、非字符串在右曾经段错误（生成器按左
# 操作数的静态类型当字符串解引用，读 len 走 +8）；Optional[str] 与 str
# 比较同样会崩。这里钉住：常量 False/True、可选字符串按内容比较、
# 同类型与 bytes/str 的既有口径不受影响。

def str_vs_int() -> bool:
    return "a" == 1

def int_vs_str() -> bool:
    return 1 == "a"

def str_ne_int() -> bool:
    return "a" != 1

def str_vs_none() -> bool:
    return "a" == None

def str_vs_true() -> bool:
    return "a" == True

def bytes_vs_int() -> bool:
    return b"a" == 1

def str_vs_float() -> bool:
    return "a" == 1.5

def opt_left(s: str | None) -> bool:
    return "a" == s

def opt_right(s: str | None) -> bool:
    return s == "a"

print(str_vs_int(), int_vs_str(), str_ne_int())
print(str_vs_none(), str_vs_true(), bytes_vs_int(), str_vs_float())
print(opt_left(None), opt_left("a"), opt_right(None), opt_right("a"))
print("a" == "a", "a" != "b", "a" == "b")
print(b"a" == "a")
# --- expect ---
# False False True
# False False False False
# False True False True
# True True False
# True
