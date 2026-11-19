# 退出 drain：未 join 的 vthread 跑完；未 join 的死亡异常打印
def ok():
    print("drained ok")

def bad():
    raise ValueError("boom")

go ok()
go bad()
print("main end")
# --- expect ---
# main end
# drained ok
# pyadamas: uncaught ValueError in unjoined vthread 2: boom
