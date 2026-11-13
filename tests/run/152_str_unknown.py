# str() 对 unknown 值的运行期分派：堆字符串原样返回，其它按整数格式化。
def show(a: list) -> None:
    x = a[0]
    print(str(x), len(str(x)))
    y = a[1]
    print(str(y), len(str(y)))
    z = a[2]
    print(str(z))

vals = []
vals.append("ab" + "cd")
vals.append(12345)
vals.append(-7)
show(vals)

def empty_len(a: list) -> int:
    e = a[0]
    return len(str(e))

blank = []
blank.append("")
print(empty_len(blank))

# --- expect ---
# abcd 4
# 12345 5
# -7
# 0
