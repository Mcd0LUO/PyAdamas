import subprocess

# Popen + wait / returncode / pid
p = subprocess.Popen(["/bin/echo", "hello"], stdout=subprocess.PIPE)
print(p.pid > 0)
print(p.wait())
print(p.returncode)

# stdout=PIPE + communicate
q = subprocess.Popen(["/bin/echo", "world"], stdout=subprocess.PIPE)
out, err = q.communicate()
print(repr(out))
print(repr(err))

# stdout 与 stderr 分开
r = subprocess.Popen(["/bin/sh", "-c", "echo out; echo err 1>&2"],
                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
o, e = r.communicate()
print(repr(o))
print(repr(e))

# stderr=STDOUT 并流
s = subprocess.Popen(["/bin/sh", "-c", "echo merged; echo also 1>&2"],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
mo, me = s.communicate()
print(repr(mo))
print(repr(me))

# stdin=PIPE + input=
t = subprocess.Popen(["/bin/sh", "-c", "cat"], stdin=subprocess.PIPE,
                     stdout=subprocess.PIPE)
co, ce = t.communicate(input="piped\n")
print(repr(co))

# shell=True 配字符串
u = subprocess.Popen("echo shell_mode", shell=True, stdout=subprocess.PIPE)
so, se = u.communicate()
print(repr(so))

# 退出码透传
v = subprocess.Popen(["/bin/sh", "-c", "exit 3"])
print(v.wait())

# kill
w = subprocess.Popen(["/bin/sh", "-c", "sleep 5"])
w.kill()
print(w.wait())

# cwd
x = subprocess.Popen(["/bin/pwd"], stdout=subprocess.PIPE, cwd="/tmp")
xo, xe = x.communicate()
print(repr(xo))

# run 的 shell / 字符串形式
res = subprocess.run("echo from_run", shell=True, capture_output=True)
print(repr(res.stdout))
print(res.returncode)
# --- expect ---
# True
# 0
# 0
# 'world\n'
# ''
# 'out\n'
# 'err\n'
# 'merged\nalso\n'
# ''
# 'piped\n'
# 'shell_mode\n'
# 3
# -9
# '/tmp\n'
# 'from_run\n'
# 0
