# try 体里的 assert 失败：lowering 走 raise 路径，新路径要把它也接到
# pa_eh_raise（否则会绕过 landing pad 直接 longjmp / 退出）。
def check(n: int) -> None:
    try:
        assert n > 0, "positive"
        print("ok", n)
    except Exception as e:
        print("assert:", e)
    print("end")

check(1)
check(-1)

# --- expect ---
# ok 1
# end
# assert: positive
# end
