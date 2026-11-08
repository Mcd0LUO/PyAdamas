# 字符串拼接、比较、遍历、f-string

def main() -> None:
    name = "py23"
    greeting = "hello, " + name
    print(greeting)
    print(len(greeting))
    print(greeting[0])
    print("abc" == "abc")
    print("abc" == "abd")
    print("apple" < "banana", "Z" < "a", "abc" >= "abd")

    n = 5
    print(f"{n} chars: {name}!")

    out = ""
    for ch in "abc":
        out = out + ch + "-"
    print(out)

main()

# --- expect ---
# hello, py23
# 11
# h
# True
# False
# True True False
# 5 chars: py23!
# a-b-c-
