# glob 常用 API 对拍（只测空目录与 escape，避免文件系统状态）。
import glob
print("empty", glob.glob("/tmp/sb7_no_such_dir_xyz/*"))
print("esc1", glob.escape("a[b]c"))
print("esc2", glob.escape("plain"))

# --- expect ---
# empty []
# esc1 a[[]b]c
# esc2 plain
