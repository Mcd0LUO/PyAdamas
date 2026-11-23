# stat 位常量与判定函数对拍。
import stat
print("bits", stat.S_IFREG, stat.S_IFDIR, stat.S_IFLNK)
print("isreg", stat.S_ISREG(stat.S_IFREG), stat.S_ISREG(stat.S_IFDIR))
print("isdir", stat.S_ISDIR(stat.S_IFDIR), stat.S_ISDIR(stat.S_IFREG))
print("perm", stat.S_IRUSR, stat.S_IWUSR, stat.S_IXUSR, stat.S_IRGRP)

# --- expect ---
# bits 32768 16384 40960
# isreg True False
# isdir True False
# perm 256 128 64 32
