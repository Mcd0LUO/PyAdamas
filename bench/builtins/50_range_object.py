# 50_range_object —— range 对象当迭代源：惰性，逐个产出
N = 1000000
r = range(N)
total = 0
for x in r:
    total += x
print(total)
