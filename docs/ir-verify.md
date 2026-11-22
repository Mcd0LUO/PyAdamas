# IR 校验器（PYADAMAS_VERIFY_IR）

> 为什么有这个东西：优化遍之间没有契约检查。一个 pass 把 IR 改坏之后，症状
> 往往出现在后面很远的地方（后端错、或者运行期段错误）。2026-11 那轮修协程
> 取消的引用计数，花了四轮才定位到一行运行时清零逻辑——如果当时有"每个 pass
> 之后检查 IR 不变量"的手段，能省掉大部分时间。
>
> LLVM 的做法是 `verifyFunction` + `-verify-each`。这里是它的极简版。

## 怎么用

```bash
PYADAMAS_VERIFY_IR=1 pyadamas run app.py    # 报告违规，继续编译
PYADAMAS_VERIFY_IR=2 pyadamas run app.py    # 报告违规并中止（退出码 70）
```

默认关闭，关闭时开销是**一次 `getenv`**（在 `optimize_ir` 前后各一次）。
检查点在 `src/lower.cpp`：`pre-opt`（降级刚结束）与 `post-opt`（优化刚结束），
所以报告里能直接看出是哪个阶段把 IR 改坏的。

## 检查什么

| # | 不变量 | 违反的后果 |
|---|---|---|
| 1 | `slot_owned[i]` ⇒ 槽类型是堆 / unknown / 已标 `slot_unknown` | 出口按指针 dec 一个整数（段错误） |
| 2 | 非生成器函数里 vreg 只能定义一次 | 值被覆盖，前面的读者拿到新值 |
| 3 | 读到的 vreg 在同一函数里有定义 | 读到栈上垃圾 |
| 4 | 恢复侧 `gen.*` 指令只出现在 `is_generator` 函数里 | 帧状态机走错分支 |
| 5 | 块以终止指令结尾；结构性终止（jump/branch/return/挂起）不出现在块中间 | 指令永不执行 / 控制流断裂 |
| 6 | 跳转目标在块表范围内；`branch` 必须有 else 目标 | 跳到别的块或越界 |
| 7 | 槽号 < `slot_ty.size()` | 读写错槽（静默错值）或越界 |
| 8 | 全局 / 字符串池 / 函数表 / 类表 / 外部符号 / 外部变量下标在表内 | codegen 按下标取表项，越界即错值 |

## 三条"看起来该查、其实不能查"的规则

写校验器最容易犯的错是把合法形态判成错误。这三条是我全量跑整个 tests/run
之后才收敛掉的，记下来免得下次又踩：

1. **生成器/协程允许 vreg 重复定义。** 跨挂起点的槽位溢出会在每个续跑块
   开头把值从堆帧槽重新装回同一个 vreg（`lower_gen_spill.cpp`）。这是既定
   形态，不是 bug。
2. **`call.rt pa_raise(...)` 出现在块中间是合法的。** `raise` 降级成 noreturn
   调用，后面可能还跟着不可达的清理指令。只有**结构性**终止指令提前出现才是
   块结构坏了。
3. **空块是合法的。** 降级会留下 `unreachable` 占位块，codegen 按 fallthrough
   处理；有人跳它也一样。

另外两条与"权威来源"有关：

- **下标归属按 `op` 判定，不按 `idx.kind`。** `Operand::kind` 是迁移期留下的
  元信息，`opt_global_promote` 把 `load.global` 改成 `load.slot` 时不会同步它，
  拿 kind 当权威会把合法指令判成越界（我第一版就踩了）。`src/ir.cpp` 里那个
  `PYADAMAS_IR_CHECK` 用的是 kind，所以它现在基本不会报。
- **外部变量和外部函数是两张表**：`CallExtern` 查 `externs`，`LoadExtern` /
  `StoreExtern` 查 `extern_vars`。混用会全量误报。

## 它到底会不会报？（自检方法）

一个从不报警的校验器等于没有。验证方法是故意破坏 IR 再跑：

```cpp
// 临时补丁：把第一个函数里所有 jump 目标改成 999
if (std::getenv("PA_VERIFY_DEMO"))
    for (auto& b : m.funcs.front()->blocks)
        for (auto& in : b.insts)
            if (in.op == IrOp::Jump) in.target = 999;
```

实测输出（补丁已回退）：

```
[verify-ir] post-opt：12 条违规
[verify-ir] __pa_main: 块 print.sep 的 jump 目标 999 越界（块数 25）
```

## 当前基线

`tests/run/` 全部用例，`PYADAMAS_VERIFY_IR=1` 下**零违规**。这既是
"没有误报"的验收标准，也意味着从今天起任何新增违规都是真回归。

## 已知未覆盖

- **重复释放**：`slot_owned` 标了两次（同一引用被两个槽/两帧登记）无法在
  单函数视图里看出来，需要跨帧/运行时配合。这正是协程取消那个 bug 的形状。
- **支配关系**：现在没有支配树，所以查不了"这条指令用了定义在非支配块里的
  值"。加支配树之后可以补。
- **类型一致**：指令 `ty` 与操作数 `vreg_ty` 的一致性还没查（需要一张逐 op
  的签名表）。
