# CFI 端到端：生成代码必须带 .eh_frame 展开规则。缺了它 libgcc 的
# _Unwind_Backtrace 会在第一个生成帧上就停下（深度 2）；有了之后能穿过
# level1/2/3、__pa_main、main 一直走到 libc 的启动帧，深度 >= 6。
cimport probe


def level3() -> int:
    return probe.probe_depth()


def level2() -> int:
    return level3()


def level1() -> int:
    return level2()


print(level1() >= 6)

# --- expect ---
# True
