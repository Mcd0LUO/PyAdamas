# 多个 vthread 阻塞在同一 channel，FIFO 唤醒
def waiter(ch, tag):
    v = ch.recv()
    print(tag, v)

ch = make_chan(int, 0)
h1 = go waiter(ch, 1)
h2 = go waiter(ch, 2)
h3 = go waiter(ch, 3)
sleep(2)
ch.send(10)
ch.send(20)
ch.send(30)
join(h1)
join(h2)
join(h3)
print("all served")
# --- expect ---
# 1 10
# 2 20
# 3 30
# all served
