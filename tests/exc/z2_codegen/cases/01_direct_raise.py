# 最小闭环：函数体里直接 raise，单层 catch-all（except Exception），
# try 之后才继续 / return。新路径只对这个形状生效。
def classify(n: int) -> str:
    r: str = "none"
    try:
        if n == 1:
            raise ValueError("boom")
        if n == 2:
            raise KeyError("k")
        r = "ok"
    except Exception as e:
        r = "caught:" + e
    return r

print(classify(1))
print(classify(2))
print(classify(3))

# --- expect ---
# caught:boom
# caught:k
# ok
