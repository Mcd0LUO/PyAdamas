# 47_open —— open/write/close + open/read/close
N = 50000
total = 0
for i in range(N):
    fh = open("/tmp/py23bench_io.txt", "w")
    fh.write("line\n")
    fh.close()
for i in range(N):
    fh = open("/tmp/py23bench_io.txt", "r")
    total += len(fh.read())
    fh.close()
print(total)
