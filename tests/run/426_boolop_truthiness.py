# and/or 的真值必须按操作数自己的类型判断。原来从结果槽按 join 类型
# （常常是 unknown）重新装载再判真，gen_branch 对 unknown 只做 cmpq $0：
# 空字符串/空列表/空字典是**非零**的堆指针，被当成真，"" or 5 返回 ""。

def str_or(s: str) -> None:
    print(s or 5)

def list_or(xs: list[int]) -> None:
    print(xs or 7)

def dict_or(d: dict[int, int]) -> None:
    print(d or 8)

str_or("")
str_or("a")
list_or([])
list_or([2])
dict_or({})
print("" or 5)
print([] or 7)
print({} or 9)
print("x" and 5)
print([1] and 7)
print("" or "z")
print([] or [4])
# --- expect ---
# 5
# a
# 7
# [2]
# 8
# 5
# 7
# 9
# 5
# 7
# z
# [4]
