# 缓冲 ping-pong N 轮 + 满阻塞让出 + 空阻塞让出
def ping(ch1, ch2, n):
    i = 0
    while i < n:
        v = ch1.recv()
        ch2.send(v + 1)
        i = i + 1

def pong(ch1, ch2, n):
    i = 0
    while i < n:
        v = ch2.recv()
        ch1.send(v + 1)
        i = i + 1

a = make_chan(int, 1)
b = make_chan(int, 1)
a.send(0)
hp = go ping(a, b, 50)
hq = go pong(a, b, 50)
join(hp)
join(hq)
print("last", a.recv())

def filler(ch):
    ch.send(10)
    ch.send(20)
    ch.send(30)
    print("filler done")

c = make_chan(int, 2)
hf = go filler(c)
sleep(5)
print(c.recv(), c.recv(), c.recv())
join(hf)
# --- expect ---
# last 100
# 10 20 30
# filler done
