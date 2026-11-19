# 1000 次 spawn/join churn + 嵌套 go 深度 20
def work(ch, i):
    ch.send(i)

ch = make_chan(int, 0)
total = 0
i = 0
while i < 1000:
    h = go work(ch, i)
    total = total + ch.recv()
    join(h)
    i = i + 1
print("churn", total)

def nest(depth):
    if depth > 0:
        h = go nest(depth - 1)
        join(h)

hn = go nest(20)
join(hn)
print("nest ok")
# --- expect ---
# churn 499500
# nest ok
