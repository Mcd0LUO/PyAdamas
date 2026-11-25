# os.walk 冒烟（0.518）：修复 heap-use-after-free 后的回归。
# 造一棵固定形状的小树，只打印与 readdir 顺序无关的数字（层数/文件数）。
import os

base = '/tmp/walk_smoke'
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
print(first)

# --- expect ---
# 4 3
# /tmp/walk_smoke
