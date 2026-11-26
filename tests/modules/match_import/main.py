# 0.646：case 分支体里的 import 必须进依赖图
def run(n: int) -> int:
    match n:
        case 0:
            import helper
            return helper.twice(n)
        case _:
            return n


print(run(5))

# --- expect ---
# 5
