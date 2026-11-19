# 只被挂起 vthread 持有的对象（含自环）强制 GC 后存活
def holder(ch):
    keep = ["held", "by", "suspended"]
    cycle = []
    cycle.append(cycle)
    ch.send(1)
    ch.recv()
    print(keep[0], keep[1], keep[2], len(cycle))

ch = make_chan(int, 0)
h = go holder(ch)
ready = ch.recv()
i = 0
while i < 40000:
    junk = ["junk" + str(i), i, i * 2]
    i = i + 1
ch.send(9)
join(h)
print("gc ok", ready)
# --- expect ---
# held by suspended 1
# gc ok 1
