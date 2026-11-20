# 增量标记正确性：10k 挂起 vthread 的栈扫描压力（每个 VT 持自环+payload，
# 挂起间大 churn 逼出 major，根扫描须覆盖全部挂起帧链）。
def holder(ch, ready, idx: int):
    cycle = []
    cycle.append(cycle)
    payload = ["vt" + str(idx), idx]
    ready.send(idx)
    ch.recv()
    result.send(payload[1] + len(cycle))

ready = make_chan(int, 10000)
release = make_chan(int, 0)
result = make_chan(int, 10000)
handles: list = []
i: int = 0
while i < 10000:
    handles.append(go holder(release, ready, i))
    i += 1
i = 0
while i < 10000:
    ready.recv()
    i += 1
j: int = 0
while j < 80000:
    junk = ["churn" + str(j), j, j * 2]
    j += 1
i = 0
while i < 10000:
    release.send(1)
    i += 1
total: int = 0
i = 0
while i < 10000:
    join(handles[i])
    total += result.recv()
    i += 1
print(total)
# --- expect ---
# 50005000
