# except / else 体里 return / break / continue 时 finally 也要执行。
# 零开销异常路径原来只在 try 体早退时注册 finally 动作：handler 体里
# return 会直接 continue、跳过 fin 块，else 体里的 return 同样跳过
# （CPython：finally 在 return 之前执行）。

def ret_in_except(k: int) -> str:
    try:
        if k == 1:
            raise ValueError("x")
        return "ok"
    except ValueError:
        return "caught"
    finally:
        print("fin", k)

def ret_in_else() -> int:
    try:
        pass
    except ValueError:
        return 3
    else:
        return 4
    finally:
        print("fin-else")

def loop_control() -> None:
    for i in range(3):
        try:
            raise KeyError("k")
        except KeyError:
            if i == 0:
                continue
            if i == 1:
                break
            return
        finally:
            print("lfin", i)

print(ret_in_except(0))
print(ret_in_except(1))
print(ret_in_else())
loop_control()
print("done")
# --- expect ---
# fin 0
# ok
# fin 1
# caught
# fin-else
# 4
# lfin 0
# lfin 1
# done
