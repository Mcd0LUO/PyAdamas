# 273 shutil 树操作：copytree（嵌套/ignore_patterns/自定义 ignore/
# dirs_exist_ok/symlinks）、rmtree（嵌套/符号链接安全/ignore_errors）、
# move（文件/入目录/目录/覆盖/自移 Error）。符号链接用 ln -s 子进程建
# （os.symlink 不在原生面）。对拍 CPython 3.14。
import shutil
import os
import os.path as osp
import subprocess
import tempfile
from typing import cast

d = tempfile.mkdtemp(prefix="pa273_")
try:
    os.makedirs(d + "/src/sub/deep", exist_ok=True)

    def wf(path: str, data: str) -> None:
        f = open(path, "w")
        f.write(data)
        f.close()

    def rf(path: str) -> str:
        # 文本口径，同 272（基线 leaf×unknown 泄漏绕行）。
        f = open(path, "r")
        s: str = f.read()
        f.close()
        return s

    wf(d + "/src/a.txt", "alpha\n")
    wf(d + "/src/sub/b.bin", "binary-content")
    wf(d + "/src/sub/deep/c.txt", "deep")

    shutil.copytree(d + "/src", d + "/tree")
    print("tree:", sorted(os.listdir(d + "/tree")))
    print("tree deep:", rf(d + "/tree/sub/deep/c.txt"))

    shutil.copytree(d + "/src", d + "/tree_ig", ignore=shutil.ignore_patterns("*.txt", "*.bin"))
    print("ignored:", sorted(os.listdir(d + "/tree_ig")), sorted(os.listdir(d + "/tree_ig/sub")))

    def my_ign(path: str, names: list[str]) -> list[str]:
        out: list[str] = []
        for n in names:
            if n == "sub":
                out.append(n)
        return out

    shutil.copytree(d + "/src", d + "/tree_ig2", ignore=my_ign)
    print("custom ign:", sorted(os.listdir(d + "/tree_ig2")))

    subprocess.run(["ln", "-s", "a.txt", d + "/src/link.txt"], capture_output=True)
    subprocess.run(["ln", "-s", "sub", d + "/src/linkdir"], capture_output=True)
    shutil.copytree(d + "/src", d + "/tree_sym", symlinks=True)
    print("sym islink:", osp.islink(d + "/tree_sym/link.txt"))
    print("sym read:", rf(d + "/tree_sym/link.txt"))
    print("sym dirlink:", osp.islink(d + "/tree_sym/linkdir"))
    shutil.copytree(d + "/src", d + "/tree_nosym")
    print("nosym islink:", osp.islink(d + "/tree_nosym/link.txt"))
    print("nosym content:", rf(d + "/tree_nosym/link.txt"))

    wf(d + "/m1.txt", "move-me")
    shutil.move(d + "/m1.txt", d + "/moved.txt")
    print("move:", osp.exists(d + "/m1.txt"), rf(d + "/moved.txt"))
    shutil.move(d + "/moved.txt", d + "/tree")
    print("move into dir:", osp.exists(d + "/tree/moved.txt"))
    shutil.move(d + "/tree_ig2", d + "/moved_tree")
    print("move dir:", osp.isdir(d + "/moved_tree"), sorted(os.listdir(d + "/moved_tree")))
    wf(d + "/ow.txt", "new")
    wf(d + "/victim.txt", "old")
    shutil.move(d + "/ow.txt", d + "/victim.txt")
    print("move overwrite:", rf(d + "/victim.txt"))

    shutil.rmtree(d + "/tree")
    print("rmtree:", osp.exists(d + "/tree"))
    os.makedirs(d + "/rmtest", exist_ok=True)
    wf(d + "/rmtest/f.txt", "keep")
    subprocess.run(["ln", "-s", d + "/src", d + "/rmtest/lnk"], capture_output=True)
    shutil.rmtree(d + "/rmtest")
    print("rmtree sym:", osp.exists(d + "/rmtest"), osp.exists(d + "/src/a.txt"))

    try:
        shutil.copytree(d + "/src", d + "/tree_sym")
    except FileExistsError:
        print("copytree FEX ok")
    shutil.copytree(d + "/src", d + "/tree_sym", dirs_exist_ok=True)
    print("dirs_exist_ok ok")
    try:
        shutil.rmtree(d + "/nope_dir")
    except FileNotFoundError:
        print("rmtree FNF ok")
    shutil.rmtree(d + "/nope_dir", ignore_errors=True)
    print("rmtree ignore ok")
    try:
        shutil.rmtree(d + "/src/a.txt")
    except NotADirectoryError:
        print("rmtree NAD ok")
    try:
        shutil.move(d + "/src", d + "/src/inner")
    except shutil.Error:
        print("move into self ok")
    wf(d + "/tree_sym/victim.txt", "x")
    try:
        shutil.move(d + "/victim.txt", d + "/tree_sym")
    except shutil.Error:
        print("move exists ok")
    print("done")
finally:
    shutil.rmtree(d, ignore_errors=True)

# --- expect ---
# tree: ['a.txt', 'sub']
# tree deep: deep
# ignored: ['sub'] ['deep']
# custom ign: ['a.txt']
# sym islink: True
# sym read: alpha
# 
# sym dirlink: True
# nosym islink: False
# nosym content: alpha
# 
# move: False move-me
# move into dir: True
# move dir: True ['a.txt']
# move overwrite: new
# rmtree: False
# rmtree sym: False True
# copytree FEX ok
# dirs_exist_ok ok
# rmtree FNF ok
# rmtree ignore ok
# rmtree NAD ok
# move into self ok
# move exists ok
# done
