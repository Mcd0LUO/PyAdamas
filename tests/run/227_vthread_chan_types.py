# channel 元素类型薄切片：float / str / unknown
def sendf(ch):
    ch.send(2.5)
    ch.send(0.125)

cf = make_chan(float, 0)
hf = go sendf(cf)
x = cf.recv()
y = cf.recv()
join(hf)
print(x + y)

def sends(ch):
    ch.send("hello")
    ch.send("world")

cs = make_chan(str, 2)
hs = go sends(cs)
join(hs)
print(cs.recv(), cs.recv())

def sendu(ch):
    ch.send(42)
    ch.send("mixed")

cu = make_chan(unknown, 0)
hu = go sendu(cu)
a = cu.recv()
b = cu.recv()
join(hu)
print(a)
print(b)
# --- expect ---
# 2.625
# hello world
# 42
# mixed
