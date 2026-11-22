# 同数组混合 coef 的读写（阶段 B5 阶段 4）：b[i*7+c] 是循环内不变下标
# （coef 0），b[j*7+c] 是跨步 7（coef 7）。读偏移 {0,1,2,6}、写偏移 {3,4,5}
# 集合不交，但两个 coef 组的地址区间必须证明不相交（入口一次性运行期守卫），
# 否则跨 lane 依赖可能撞车 → 保守标量。nbody advance 的内层就是这种形状。
#
# 覆盖：多组不变量、AccStore（coef0 原地累加）+ scatter（coef7 跨步写）混合、
# 顺序敏感浮点（与 CPython 逐位比对）、越界 IndexError（守卫失败 → 标量尾）。
import math


def pairwise(b: list[float], dt: float) -> None:
    for i in range(5):
        for j in range(i + 1, 5):
            dx = b[i * 7] - b[j * 7]
            dy = b[i * 7 + 1] - b[j * 7 + 1]
            dz = b[i * 7 + 2] - b[j * 7 + 2]
            d2 = dx * dx + dy * dy + dz * dz
            mag = dt / (d2 * math.sqrt(d2))
            b[i * 7 + 3] -= dx * b[j * 7 + 6] * mag
            b[i * 7 + 4] -= dy * b[j * 7 + 6] * mag
            b[i * 7 + 5] -= dz * b[j * 7 + 6] * mag
            b[j * 7 + 3] += dx * b[i * 7 + 6] * mag
            b[j * 7 + 4] += dy * b[i * 7 + 6] * mag
            b[j * 7 + 5] += dz * b[i * 7 + 6] * mag


def run(seed: float) -> None:
    b = []
    for k in range(35):
        b.append(seed + float(k) * 0.25 - 4.0)
    pairwise(b, 0.01)
    print("run", seed, b[3], b[10], b[17], b[24], b[31], b[34])


for seed in (0.0, 1.5, -2.25, 0.125, -0.75):
    run(seed)

# 越界：b 只有 20 个元素，pairwise 会读到 b[34] → IndexError（守卫失败走标量尾，
# 异常类型与时机与标量一致）。
try:
    short = []
    for k in range(20):
        short.append(float(k) * 0.5 + 1.0)
    pairwise(short, 0.01)
    print("no raise")
except IndexError:
    print("IndexError")

# --- expect ---
# run 0.0 -3.2499454506548386 -1.4970543353612773 0.2532991443953693 2.0024918140869783 3.7482653308238634 4.5
# run 1.5 -1.7486035367638646 0.0032860525525305237 1.7532991443953692 3.5021514261731705 5.246923416932891 6.0
# run -2.25 -5.501958321491299 -3.747564917231989 -1.9967008556046308 -0.24699760404230944 1.5002782016603238 2.25
# run 0.125 -3.124833624497257 -1.3720259697017936 0.3782991443953693 2.1274634484274944 3.8731535046662824 4.625
# run -0.75 -4.000616407600326 -2.247224529318181 -0.49670085560463073 1.2526620080438828 2.99893628776935 3.75
# IndexError
