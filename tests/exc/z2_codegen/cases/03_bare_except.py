# 裸 except（无条件 catch-all）：与 except Exception 共用同一种 LSDA 形态，
# 区别只在 landing pad 后的分发块不做类型判断。
def swallow() -> None:
    try:
        raise RuntimeError("boom")
    except:
        print("bare caught")
    print("done")

swallow()

# --- expect ---
# bare caught
# done
