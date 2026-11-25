# os.walk 冒烟（0.518）：修复 heap-use-after-free 后的回归。
# 造一棵固定形状的小树，只打印与 readdir 顺序无关的数字（层数/文件数）。
# 目录名带 pid、跑完自清理：用例必须可重复跑，不能依赖 /tmp 里有没有残留。
import os

base = 'walk_smoke_' + str(os.getpid())
os.mkdir(base)
os.mkdir(base + '/a')
os.mkdir(base + '/b')
os.mkdir(base + '/a/c')
f = open(base + '/f0.txt', 'w')
f.write('x')
f.close()
f = open(base + '/a/f1.txt', 'w')
f.write('y')
f.close()
f = open(base + '/a/c/f2.txt', 'w')
f.write('z')
f.close()

levels = 0
nfiles = 0
first = ''
for root, dirs, files in os.walk(base):
    levels += 1
    nfiles += len(files)
    if levels == 1:
        first = root
print(levels, nfiles)
print(first == base)

# 自清理（walk 已跑完，按已知形状自底向上删）
os.unlink(base + '/a/c/f2.txt')
os.rmdir(base + '/a/c')
os.unlink(base + '/a/f1.txt')
os.rmdir(base + '/a')
os.rmdir(base + '/b')
os.unlink(base + '/f0.txt')
os.rmdir(base)

# --- expect ---
# 4 3
# True
