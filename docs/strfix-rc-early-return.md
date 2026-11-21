# configparser 插值字符串 refcount 泄漏取证（strfix / 0.356）

## 现象

最小复现 `/tmp/r392.py`：

```python
from configparser import ConfigParser
c = ConfigParser()
c.read_string('[a]\nslash = %(third)s/%%plit\nthird = end')
print(c.items('a'))
```

输出正确 `[('slash', 'end/%plit'), ('third', 'end')]`，但 `--asan` +
`ASAN_OPTIONS=detect_leaks=1` 报 **208B / 2 对象**（各 104B）。无异常参与
（EXC_TRACE 空）、门控关也复现、GC 不参与（runs=0）。0.355 异常 leak 修
对其无影响（基线同值），batch7 归因「异常 RC 环」不准确。

## 取证（ASan + gdb）

104B = 64B `PyAdamasRc` 头 + 40B `PyAdamasStr`（SSO，len ≤ 7，字符内联在
hash 字段）。字符串在 exact-RC 模式下是**索引叶子**（`PYADAMAS_RC_LEAF`）：
不进 GC 堆链，只在 `leaf_page_tab` 位图登记表里（位图不存对象指针）。
rc 永不归零的字符串退出时仍 malloc 着、又无任何活指针引用 → LSan 报
Direct leak。LSan 栈指向 `_read:441/513` 是**字符串池复用**造成的假象
（块最初在那里 malloc，释放回池后被插值串复用）。

gdb 在 `pa_rc_shutdown` 入口遍历 `leaf_page_tab` 位图，退出时仅存 2 个
非永生 STR（rc=1）：

| 对象 | len | 内容 | 最后分配点（gdb FinishBreakpoint 追 rc_alloc_core） |
|------|-----|------|--------------------------------------------------|
| 0x…0460 | 5 | `third` | `optionxform` → `optionstr.lower()`（configparser.py:308），调用自 `_interp_some:696` `var = self.optionxform(var)` |
| 0x…07d0 | 3 | `end` | `_read:530` 提交 `"\n".join(src[k]).rstrip()`（op=10 rstrip），即字典 `_sections['a']['third']` 的值 |

## 根因：编译器 lowering 早返回漏 dec（通用 bug）

`_interp_some` 的 while 循环里 `if p < 0: accum.append(rest); return`
（configparser.py:676）是一条**早返回**。反汇编（configparser 模块 mod.s）
对比两条出口：

- **循环自然结束出口**（bb5）：`pa_rc_dec` s4(rest) / **s8(var,144)** /
  **s11(v,168)** / s12，四个槽全释放。
- **早返回出口**（bb7，.loc 676）：只 dec s4 / s12，**漏 s8(var) 与 s11(v)**。

机理：`Lowerer::release_slots()`（src/lower_impl_slots.cpp）按槽的**静态
属性**（`slot_owned` / `slot_ty` / `slot_unknown`）决定出口释放哪些槽。
降级是**单遍源码序**：早返回（676 行）在 `var`/`v` 首次堆/owned 写入
（688/704 行）**之前**降级，此刻这两个 unknown 槽的 `slot_owned` 尚未置位
→ `release_slots` 整类跳过。循环自然结束出口在循环体之后降级，标记已置位
→ 全释放。槽在序言清零，循环次轮起早返回时槽里已是上一轮 `store_slot`
retain 过的堆值：

- s8 = var = optionxform 结果 `third`（自有引用）→ 直接泄漏。
- s11 = v = `d[var]` = `end`（字典借来 + retain）→ 字典值多持一份，退出
  字典释放一次后 rc 仍 1 → 泄漏。

触发形状（实测）：当且仅当 `_interp_some` 处理过 `%(var)s` 后经 `p < 0`
分支早返回才漏。`%(third)sx` / `%(third)s/%%plit` 漏；`%(third)s`（rest
空，循环自然结束）/ `%(third)s%%` / `%(third)s/%(third)s` / `%%/%(third)s`
（均循环自然结束）/ 无 var 不漏。

## 通用编译器修为何本轮不做（负面）

已实现并验证一个编译器后置遍 `fixup_early_return_releases`：降级完成后用
**最终**槽属性补齐早返回出口缺失的 dec（只补缺失、已完整出口不动；生成器/
协程跳过）。复现 leak→0、输出不变。但 **38 热基准 asm 对拍 37 同 1 异**：
`bench/daily/a2_config_parse.py` 的 `coerce()` 自身两个早返回同样漏 dec s2，
通用修给它补了 `pa_rc_dec_unknown(s2)`（一处真实潜伏泄漏）→ asm 改变，
**违反「flag-off 38 基准 asm 逐字节不变」硬闸门**。

结论：这是编译器字符串临时量「早返回漏 dec」通用 bug，正确修法在 lowering
（出口释放需用最终槽属性，而非单遍降级当时的部分标记），但会改动含此形状
的现有程序 asm，须独立任务连同基准 asm 基线一并处理。本轮按硬闸门走**纯
stdlib 修**（configparser 是独立模块，38 基准 entry asm 天然不变），编译器
bug 留负面待修。

## 本轮 stdlib 修

见 CHANGELOG 0.356：把 `_interp_some` 的 `p < 0` 早返回 `return` 改成
`break`，经循环自然结束出口（全槽释放）退出，语义不变（循环后无语句）。
