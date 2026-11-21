# expect: big shift requires a statically typed integer count
a: big = 300
n: object = 5
print(a << n)
print(a >> n)
a <<= n
