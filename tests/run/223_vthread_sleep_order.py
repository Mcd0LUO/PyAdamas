# sleep 让出：交错顺序由 deadline 确定
def nap(name, ms):
    sleep(ms)
    print(name)

h1 = go nap("a", 30)
h2 = go nap("b", 10)
h3 = go nap("c", 20)
sleep(40)
join(h1)
join(h2)
join(h3)
print("awake")
# --- expect ---
# b
# c
# a
# awake
