# Windows(win64) 移植 —— 公共上下文

## 项目
PyAdamas：Python 语法的静态编译器。C++23 前端产出 x86-64 AT&T 汇编，再调用系统 cc
汇编链接。运行时有 ~15k 行 C11。当前支持 Linux(x86-64 SysV)。最近新增了
Windows(win64) 目标，通过 --target=win64 选择。

## 关键机制
- `src/backend/target_abi.h`：TargetABI 结构描述一个目标的 ABI 事实（实参寄存器、
  callee-saved 池、浮点池、scratch、栈对齐、影子空间、setjmp 符号名等）。
  两个实例：`kX86_64SysV`（默认）与 `kX86_64Win64`。
- 生成代码的实参一律走 `arg_reg_i(n)` / `arg_reg_f(n)`（按 ABI 取寄存器）或
  `emit_arg(pos, is_fp, cls_idx, v)`（同时支持 SysV 的按类型计数与 Win64 的按位置分配，
  超出寄存器个数的落栈）。
- Win64 与 SysV 的主要差异：
  * 整型实参只有 4 个（rcx/rdx/r8/r9），SysV 6 个；浮点 4 个 vs 8 个
  * Win64 按**位置**分配（第 i 个实参固定第 i 个槽），SysV 按**类型分计数**
  * 调用点必须预留 32 字节影子空间（`out_base()`）
  * rdi/rsi 在 Win64 是 callee-saved，SysV 是实参寄存器
  * setjmp/longjmp 用自带的 pa_win_setjmp/pa_win_longjmp（见 runtime/rt_exc.c），
    因为 msvcrt 的 longjmp 走 SEH 展开而我们的汇编函数没有 .pdata
- 运行时平台层在 `runtime/rt_internal.h`（poll -> WSAPoll、clock_gettime ->
  QueryPerformanceCounter 等）。

## 构建与验证（务必按此顺序）

```bash
# 1) 构建编译器（本 worktree 内）
cmake --preset dev > /dev/null 2>&1   # 只需一次
ninja -C build/dev -j6 pyadamas

# 2) 若改了 runtime/*.c，重建 Windows 版运行时
./wintest.sh winrt

# 3) 编译并运行某个用例到 Windows
D=/mnt/c/Users/lenovo/AppData/Local/Temp/winrun
./build/dev/pyadamas --rebuild asm --target=win64 tests/run/XX.py > $D/xx.s
x86_64-w64-mingw32-gcc -c $D/xx.s -o $D/xx.o
x86_64-w64-mingw32-gcc $D/xx.o /tmp/winrt/rt_win.o -o $D/xx.exe -lws2_32
timeout 10 $D/xx.exe; echo "EXIT=$?"
```
WSL 可以直接执行 .exe 并拿到真实退出码，不需要 wine/PowerShell。

## 硬约束（违反即失败）

1. **SysV 输出必须逐字节不变**。做完改动后必须验证：
   ```bash
   mkdir -p /tmp/asmchk && rm -f /tmp/asmchk/*
   for f in tests/run/*.py; do b=$(basename "$f" .py); ./build/dev/pyadamas asm "$f" > /tmp/asmchk/$b.s 2>/dev/null; done
   cat /tmp/asmchk/*.s | md5sum
   ```
   基线 md5 = `88f2bb28da857d385fae0f100866782a`（160 个用例）。默认目标下所有
   改动都应该是恒等替换；若 md5 变了，说明你动了 SysV 行为，必须改回来。
2. **POSIX 测试必须保持 177 通过 / 0 失败**：
   `PYADAMAS_TEST_JOBS=4 bash tests/run_tests.sh build/dev build/dev/pyadamas`
3. **不要跑全量 ASan**（太慢，会拖垮机器）。需要内存诊断时用局部 `-fsanitize=address`
   单独编一个文件，或只跑单个用例。
4. 不要改 `tests/` 下已有用例的期望输出；新增用例可以。
5. 只改你负责的失败面，不要顺手重构无关代码。
6. 完成后在你的分支上 `git add -A && git commit`（不要 push，不要切分支，不要动 master）。

## 已知同类问题的修法模式（供参考）
- 汇编里出现 `%rdi/%rsi/%rdx/%rcx/%r8/%r9` 当作实参或基址寄存器 → 改成
  `arg_reg_i(n)` 或 `emit_arg(...)`，并用 `line(std::format(...))` 拼串。
- 汇编里出现 `%rdi` 既当实参又当基址 → 说明需要引入独立的 scratch 寄存器，
  参考 TargetABI 的 scratch_len/scratch_ptr/scratch_val。
- 运行时的回调/跳板（手写 asm）→ 参考 runtime/rt_functools.c 的 pa_call2。
