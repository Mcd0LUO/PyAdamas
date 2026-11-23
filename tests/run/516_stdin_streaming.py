# 516_stdin_streaming.py —— 流式 stdin/管道读取（0.447）
# 回归：glibc 的 fread 会为凑满请求量循环 read(2)，写端不关就阻塞到 EOF。
# 文本模式预读改为流式 fd 上一次 read(2)：readline 拿到行就返回、read(n)
# 继续要直到凑满 n（CPython buffered 语义）、read() 到 EOF。
import subprocess
import time

# 1) 子进程写一行后保持管道开着 1s：readline 必须立刻返回（< 0.5s）。
p = subprocess.Popen(["sh", "-c", "printf 'hello\n'; sleep 1"], stdout=subprocess.PIPE)
t0 = time.time()
line = p.stdout.readline()
dt = time.time() - t0
print("readline fast:", dt < 0.5)
print("readline:", len(line), line)
p.wait()

# 2) read(6) 精确读：字节分两批、间隔 0.3s 到达，必须凑满 6 字节再返回。
p = subprocess.Popen(["sh", "-c", "printf 'abc'; sleep 0.3; printf 'def'; sleep 0.3"],
                     stdout=subprocess.PIPE)
s = p.stdout.read(6)
print("read6:", len(s), s)
p.wait()

# 3) read() 读到 EOF（写端分两次写、最后关闭）。
p = subprocess.Popen(["sh", "-c", "printf 'xy'; sleep 0.2; printf 'z'"], stdout=subprocess.PIPE)
all_s = p.stdout.read()
print("read all:", all_s)
p.wait()

# 4) for line in 文件对象：逐行迭代到 EOF。
p = subprocess.Popen(["sh", "-c", "printf 'a\nb\nc\n'"], stdout=subprocess.PIPE)
n = 0
for ln in p.stdout:
    n += 1
print("iter lines:", n)
p.wait()

# 5) write + flush：Popen 管道写端不 flush 时子进程（cat）收不到；flush 后立即可读。
p = subprocess.Popen(["/bin/cat"], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
p.stdin.write("ping")
p.stdin.flush()
print("flush:", p.stdout.read(4))
p.stdin.close()
p.wait()

# --- expect ---
# readline fast: True
# readline: 6 hello
#
# read6: 6 abcdef
# read all: xyz
# iter lines: 3
# flush: ping
