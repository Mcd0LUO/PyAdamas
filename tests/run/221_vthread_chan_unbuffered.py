# 无缓冲 channel：生产者/消费者握手，值按序转移
def producer(ch, n):
    i = 0
    while i < n:
        ch.send(i * i)
        i = i + 1

ch = make_chan(int, 0)
h = go producer(ch, 6)
total = 0
i = 0
while i < 6:
    v = ch.recv()
    total = total + v
    i = i + 1
join(h)
print("sum", total)
# --- expect ---
# sum 55
