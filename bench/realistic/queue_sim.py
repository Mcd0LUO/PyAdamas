# queue_sim —— 事件队列模拟（列表/字典混合，真实程序形态）
#
# 修正版：原版 pending 从不清空，while 每轮扫全表 -> O(steps^2)，
# 测出来的不是队列语义而是「平方级扫描 + 索引访问」的复合成本。
# 这里让事件在到期时真正出队，队列长度稳定在 O(1)，扫描是 O(n) 线性。
N = 26000

def simulate(steps: int) -> int:
    pending = []
    done = 0
    for t in range(steps):
        if t % 3 == 0:
            pending.append((t, t % 7 + 1))
        # 从队头出队：已到期的事件全部弹出。
        # 用「读 + 覆盖 + 截断」模拟队列前进，避免 O(n^2) 的中间删除。
        head = 0
        n = len(pending)
        while head < n:
            item = pending[head]
            if item[0] + item[1] <= t:
                done += 1
                head += 1
            else:
                break
        if head > 0:
            rest = []
            for k in range(head, n):
                rest.append(pending[k])
            pending = rest
    return done + len(pending)

total = 0
for r in range(N):
    total += simulate(200)
print(total)
