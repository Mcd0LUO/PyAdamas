# stdlib/sigs/os.pysig 迁移回归：模块级函数 + 标量常量
# 与 CPython 3.14 同形脚本对拍；PA 专有语义（getenv 默认值）如实记录。
import os
import os.path as osp
import shutil

print("consts", os.F_OK, os.X_OK, os.W_OK, os.R_OK)
print("sep", repr(os.sep))
print("linesep", repr(os.linesep))
print("getcwd", isinstance(os.getcwd(), str))
print("getpid", os.getpid() > 0)
print("cpu_count", os.cpu_count() > 0)
print("getenv", os.getenv("PATH") != "")
print("getenv_default", repr(os.getenv("__SB5_NO_SUCH_VAR__", "")))

# 文件系统操作（用临时目录，对拍文件存在性而非路径字符串）
d = "/tmp/sb5_os_pysig_528test"
shutil.rmtree(d, ignore_errors=True)
os.mkdir(d)
print("mkdir", osp.isdir(d))
os.makedirs(d + "/a/b")
print("makedirs", osp.isdir(d + "/a/b"))
f = d + "/t.txt"
w = open(f, "w")
w.write("hi")
w.close()
print("listdir", sorted(os.listdir(d)))
print("access", os.access(f, os.F_OK), os.access(f, os.R_OK))
os.rename(f, d + "/t2.txt")
print("rename", osp.exists(d + "/t2.txt"), not osp.exists(f))
os.remove(d + "/t2.txt")
print("remove", not osp.exists(d + "/t2.txt"))
shutil.rmtree(d)
print("cleanup", not osp.exists(d))

# from import
from os import getpid, F_OK
print("from_import", getpid() == os.getpid(), F_OK == os.F_OK)

# --- expect ---
# consts 0 1 2 4
# sep '/'
# linesep '\n'
# getcwd True
# getpid True
# cpu_count True
# getenv True
# getenv_default ''
# mkdir True
# makedirs True
# listdir ['a', 't.txt']
# access True True
# rename True True
# remove True
# cleanup True
# from_import True True
