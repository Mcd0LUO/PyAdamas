# 29_dict —— dict(关键字)：py23 只支持关键字形式
N = 200000
total = 0
for i in range(N):
    c = dict(k0=i, k1=1, k2=2, k3=3, k4=4, k5=5, k6=6, k7=7)
    total += c["k0"]
print(total)
