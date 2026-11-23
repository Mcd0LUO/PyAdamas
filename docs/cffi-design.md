# C FFI 下一步先验：免写直调 + dlopen（审计 / 设计 / 薄原型）

> **0.306 改名**：`cdll` → `dylib`（平台中性命名——DLL 是 Windows 术语，本功能是 dlopen/LoadLibrary 的跨平台 facade；ctypes 的 cdll 本意是 cdecl/stdcall 调用约定区分，本项目无此区分，名字理据不成立）。下文示例与标识符均已更新；测试目录 tests/dylib/、用例 213_dylib.py。

分支 `cffi`（基于 f0a4af4 / 0.303），独立 worktree 先验，未推送。
目标三件：① 健壮化现有 cimport/cdecl；② 免写声明直调 C 函数；③ dlopen/dlsym
动态库能力。本文是审计结论 + 设计 + 原型实测 + 坑位清单；原型薄但端到端
（O0/O2 均验证），深坑记录绕行，不追求完备。

---

## 1. 现状审计

### 1.1 管线（cimport / cdecl）

```
cimport clib as c          parser_stmt.cpp: parse_cimport
  → load_cpya("clib")      cimport.cpp: 手写 .cpya 文本声明（cmodule/link/
                           const/struct/extern var/extern def），不是 C 头文件
  → stmt_cimport           typecheck_stmt_forms.cpp: 建 FuncInfo(is_extern)，
                           CKind → TypePtr（ckind_type），struct 布局进 structs_
  → c.f(...)               call_attr 按 extern_fns_ 查签名 → check_args
  → lower_cimport_call     lower_method.cpp: 逐实参 coerce + 选 CKind（ptr 接
                           list/bytes 时改 Buf*），登记 IRExtern（按 symbol 去重），
                           发 CallExtern（cargs 逐调用点记编组）
  → gen_call_extern        codegen_call.cpp: SysV 分类（6 int / 8 xmm / 栈溢出 /
                           结构体 chunk / 隐藏 sret / 回调跳板 / varargs %al），
                           call <symbol> —— **链接期绑定**
  → link_program           driver_build.cpp: cc objs rt.o -lm + .cpya 的 link 行
```

`pyadamas cdecl header.h -o m.cpya --link ...` 是另一条入口：cdecl.cpp
（词法/预处理）+ cdecl_parse.cpp（类型/struct/enum/声明）把 **C 头文件子集**
转成 .cpya 文本，之后走同一条管线。

### 1.2 已支持（审计确认）

- CKind：Int(i64)/Int32/UInt32/Float(double)/Bool/Str/OptStr/Ptr/Struct/Callback，
  lower-only 的 BufF64/BufI64/BufBytes（list/bytes 连续 data 直传）。
- 结构体按值：.cpya 定宽标量字段，编译期 C 布局；SysV chunk 分类
  （IRStructABI.chunk_fp），>16B 走 MEMORY/隐藏指针；返回值 rax/rdx/xmm0/xmm1
  分块暂存后写回 pa_struct_new payload。
- 回调：fn(...) 签名 → 每签名一个跳板 .Lcbtramp + 一个全局闭包槽 .Lcbslot；
  调用点登记闭包、传跳板地址；跳板做 32 位/bool 提升与 C ABI↔统一约定重排。
- 变参：声明末尾 `...`，额外实参按静态类型选编组，%al = 用到的 xmm 数。
- extern var：按宽度 RIP 相对加载/存储；str 全局复制成 PyAdamasStr。
- ptr[T] 解引用：p[i] 按 ckind_width 比例寻址，UInt32 零扩展。
- str 边界：实参取 PyAdamasStr.data（+24，**NUL 终止不变量**，str_alloc_raw
  保证，SSO 也终止）；返回 char* 复制（NULL→"" / Optional 保持 None）。

### 1.3 已知限制（roadmap 技术债 14 + 本轮审计）

| 限制 | 状态 |
| --- | --- |
| cdecl 不展开函数式宏/条件编译 | 记录，不做（深坑：需真预处理器，考虑 libclang） |
| C float 参数/返回整体跳过 | **本轮修复**：新增 CKind::Float32（§5.1） |
| 位域 / 匿名 struct 字段 | 记录：降级 opaque / 跳过（布局依赖编译器，不做） |
| union 按值、定长数组字段 | **已支持**（P1/P2）：`@union` / `T[N]` 字段、`@packed`；按值 ABI 仍只覆盖纯标量/指针字段的 struct |
| 回调槽不可重入（每签名一个全局槽） | 记录，不做（需 TLS 或逐调用点槽 + 生命周期） |
| 回调不转发栈参（>6 int / >8 fp 拒绝） | 记录 |
| 函数指针变量 / 函数指针 typedef | cdecl 跳过并警告 |
| 结构体字段仅定宽标量 | float32 字段本轮**仍拒绝**（显式报错，见 §5.1 边界） |
| 无 dlopen/dlsym | **本轮新增**（§3） |
| 变参 %al 未钳制到 8 | **本轮修复**：fi>8 时 %al=8（ABI 上限，溢出浮点走栈） |
| 回调签名含 C float | 本轮加显式 guard：cdecl 跳过并警告（跳板不做单精度提升） |

另：Debug 构建下 `118_suspend_expr` / `154_nested_await` 触发
`vreg_nonneg` 的 STL 越界断言（lower_impl.h def()，生成器路径）——
**在 base f0a4af4 的独立 worktree 同样复现**，与本轮 FFI 改动无关，
0.303 的 225 例套件应是 Release 跑的。单独开票，不在本轮范围。

## 2. codon 参考（只读 .ref_proj/codon）

codon 没有 cimport 关键字，C 互操作全在 import 语法里：

- `from C import foo(int) -> float as f` → transformCImport
  （parser/visitors/typecheck/import.cpp）：合成带 Attr::C 的函数声明，
  LLVM 直发符号调用（链接期绑定）。**签名必须写在 import 处**。
- `from C import foo: int` → C 变量（LinkType / ExprExternVar）。
- `from C import lib.foo(int) -> float` → transformDYLIBImport：
  `_dlsym_c(lib, "foo", Fn=Function[[int], float])`。
- stdlib/internal/dlopen.codon：用 `from C import dlopen/dlsym/dlclose/dlerror`
  自举包装；dlsym 返回 `Fn(fn)`（指针→函数类型转换），错误抛 CError(dlerror())；
  Windows 用 _CFunction + @llvm_c_call 适配 C ABI。

对 pyadamas 的启示：dlsym 后端与 codon 同构（本轮 rt_cffi.c 即此形态）；
但 codon 要求 import 处写签名，本轮的免写直调更进一步——**签名由调用点
推断/标注**，更接近 ctypes 的契约（无声明 = 调用方对 ABI 负责）。

## 3. 语法与语义（原型已实现）

### 3.1 免写直调

```python
from c import sqrt, strlen        # 绑定默认库符号，不写任何 cdecl/.cpya
from c import pow as cpow         # 别名：C 侧符号名仍是 pow
print(sqrt(2.0))                  # 1.4142135623730951
```

- `c` 是伪模块（kModuleTable 登记 Native，无源文件、无 init、globals 空）。
  符号名集合编译期不可知，绑定在 stmt_import_from 特判：每个名字建
  FuncInfo{c_lazy, c_symbol}，declare 成 Func 类型。
- 调用点（check_c_lazy）：拒绝关键字/`*`展开；求实参类型；推断返回 ABI。
- 降级（lower_c_lazy.cpp）：逐实参推断 CKind + coerce，按
  (符号, 绑定方式, 返回种类) 去重登记 IRExtern{lazy=1}，发 CallExtern。
- 符号解析：首次调用 `dlsym(RTLD_DEFAULT, name)`（进程全局符号表，
  含已链接的 libc/libm 与所有 RTLD_GLOBAL 库），结果缓存到静态槽
  .Lcsym<N>；此后调用点只多一次 load+test+jne 与间接 call。
- `from c import *` 拒绝；`import c; c.sqrt(...)` 不支持（明确报错，
  指引 from-import）；c_lazy 函数**不能当值**（传递/存容器）——没有
  固定签名就没有统一约定入口，记录为后续（§7）。

### 3.2 dlopen 动态库

```python
import dylib
lib = dylib.load("./libprobe.so")  # dlopen(RTLD_NOW|RTLD_LOCAL) → 句柄
print(lib.probe_add(2, 3))        # 句柄属性调用 = dlsym(句柄, "probe_add")
s: str = lib.probe_greet("hi")    # char* 返回用标注定 ABI
print(dylib.close(lib))            # dlclose → 0
```

- `dylib` 伪模块：load(str)→DYLIB 句柄、close(DYLIB)→int，走原生函数
  管线（native_fn → RtFn::DYLIBLoad/DYLIBClose → rt_cffi.c）。
  `from dylib import load` 同样可用（native_aliases_）。
- 句柄类型 TyKind::DYLIB：**标量整数地址**（非堆、无 RC），可存容器、
  可传参；print 输出 `<dylib handle>`；不跨模块序列化（enc_type→unknown）。
- 句柄属性调用与 from c import 共享 lower_c_lazy_call / CallExtern /
  gen_call_extern，仅绑定方式不同：IRExtern.lazy=2，句柄值在 Inst::a，
  每次调用 `dlsym(handle, name)`（原型不做 (handle,symbol) 缓存）。
- 错误路径（全部 OSError，消息点名符号/路径，绝不静默跳空指针）：
  - 库不存在：`dylib.load: cannot open '<path>': <dlerror>`
  - 默认符号不存在：`undefined C symbol '<name>' (dlsym RTLD_DEFAULT)`
  - 句柄符号不存在：`undefined C symbol '<name>' in dylib handle`
  - dlsym 的 NULL 用 dlerror() 区分（符号值合法为 0 的边界）。

## 4. 类型规则与 ABI 契约

### 4.1 实参推断（免写路径，lower_c_lazy.cpp lazy_arg_kind）

| PyAdamas 静态类型 | C 编组 | 机器动作 |
| --- | --- | --- |
| float | double | xmm（movsd） |
| int / bool | i64（bool 传 0/1） | 整型寄存器 |
| str | char* | 取 data(+24)，NUL 终止不变量 |
| bytes | uint8_t* | 取 data(+24) |
| list[float] / list[int] | double* / i64* | 取 list data(+16) |
| Optional[str] | 可空 char* | NULL 原样传 |
| Optional[int]（C ptr） | void* | 整数地址 |
| 其它 / unknown | **build 期报错** | "cannot infer C ABI ... annotate or use cimport" |

### 4.2 返回推断与标注覆盖

- 默认：有 float 实参 → double ABI；否则 i64 ABI（ctypes 默认 restype=c_int
  的同构契约）。**无标注时指针返回会按 i64 读出垃圾值**——与 ctypes 一致，
  文档写明，测试 213 用标注演示正确姿势。
- 标注覆盖（c_abi_annotation）：`x: float = f(...)`、`x: str = lib.g(...)`、
  `def h(...) -> float: return f(...)`。支持 float/int/str/bool/None/
  Optional[str|int]。实现：stmt_ann_assign / stmt_return 在求值后把
  expr_types_[call] 覆写成标注类型，降级据此选返回编组。
- 契约边界（免写路径**不做**，需 cimport .cpya）：C float 单精度、
  结构体按值、变参、回调、>6 int/>8 fp 的栈参转发（免写路径栈溢出
  实参仍按 SysV 落栈，但无声明校验）。float 实参永远按 double 传——
  调 C float 函数请用 float32 声明（§5.1），否则 ABI 错（实测演示见
  先验记录：probe_f32 经免写路径读回垃圾）。

### 4.3 SysV 要点（沿用既有编组）

6 整型 / 8 xmm，溢出走 out_base 栈槽；结构体 chunk 分类与隐藏 sret 不变；
变参 %al = min(用到的 xmm 数, 8)（本轮钳制）；免写路径新增 fn_tmp 暂存
（out_base+stack_bytes+48），outgoing_bytes 暂存上界 48→56。

## 5. 健壮化修复（本轮挑的便宜洞）

### 5.1 C float（float32）编组 —— 修复

- CKind::Float32（ckind_width=4）；.cpya 类型名 `float32`（`float` 仍是
  double，兼容不变）。PyAdamas 侧统一 double（ckind_type→t_float）。
- 边界转换：参数 cvtsd2ss（寄存器/栈两路），返回 cvtss2sd；
  extern var float32 的 movss 加载/存储；ptr[float32] 解引用
  （fwidth=4 + movss/cvtss2sd，写回 cvtsd2ss+movss）。
- cdecl：C `float` 参数/返回现在生成 float32（原先整体跳过函数）；
  tests/cdecl/sample.h 增加 scalef/global_ratiof 端到端验证。
- **边界（刻意不做）**：float32 结构体字段（cfield_layout 拒绝，显式报错）、
  含 float32 的回调签名（callback_kind_ok 拒绝；cdecl 跳过并警告）。
  这两处涉及字段访问宽度/跳板提升，深度超出本轮，记录于 §7。
- 变参里的 float 仍按 double（C 默认实参提升，语义正确）。

### 5.2 变参 %al 钳制 —— 修复

fi>8（浮点实参溢出到栈）时 %al 原样塞 fi 会让 va_arg 寄存器保存区越界读；
钳到 ABI 上限 8。一行修复，111_varargs 回归通过。

## 6. 落地路径与实测

### 6.1 改动清单

| 层 | 文件 | 内容 |
| --- | --- | --- |
| 类型 | typecheck_types.h | TyKind::DYLIB / t_dylib / FuncInfo.c_lazy+c_symbol / ckind_type(Float32) |
| 类型检查 | typecheck_stmt_forms.cpp | from c import 绑定；标注覆盖（ann_assign/return） |
| 类型检查 | typecheck_call.cpp / typecheck_call_forms.cpp | check_c_lazy；DYLIB 接收者属性调用 |
| 模块 | driver_build.cpp / builtin_sigs.cpp / lower_import.cpp | c/dylib 伪模块登记；native_fn；-ldl |
| 降级 | lower_c_lazy.cpp（新）/ lower_call.cpp / lower_method.cpp / lower_import_native.cpp | 调用点编组推断 + 分派 |
| IR | ir_types.h / ir_ops.h / codegen_call_rt.cpp | IRExtern.lazy/sym_name；IRModule.c_lazy_syms；RtFn::DYLIBLoad/Close |
| 发射 | codegen_call.cpp / codegen.cpp / codegen_block.cpp | dlsym 解析序列 + 间接 call；float32 编组；符号串/缓存槽；暂存上界 |
| 运行时 | rt_cffi.c（新）/ rt_internal.h / CMakeLists.txt | dlopen/dlsym 后端（Win32 给 NotImplementedError 桩） |
| 序列化 | module.cpp | DYLIB 跨模块按 unknown |
| 测试 | tests/run/212–215、tests/dylib/probe.c、tests/fail/f27、tests/strict/s04、tests/cdecl/sample.* | 见下 |

### 6.2 实测（Debug 编译器，生成程序 O0/O2 均验证）

- **212_c_lazy**：from c import sqrt/pow/strlen/labs/getpid，零声明。
  `sqrt(2.0)` → `1.4142135623730951`（O0==O2）；别名、标注、函数返回
  标注、循环内重复调用（缓存槽）均正确。
- **213_dylib**：subprocess 现场 `cc -shared -fPIC -O2 tests/dylib/probe.c`
  建 .so（构建说明在 probe.c 头部），dylib.load → probe_add(2,3)=5、
  probe_scale(4.0)=10.0、**int/str 混合** probe_mix(10,"hello")=15、
  str 标注 probe_greet→"hi"、close→0。
- **214_c_lazy_errors**：三类错误路径消息（§3.2），O0==O2。
- **215_c_float32**：f32_mul(1.5,2.25)=3.375、f32_of_int(7)=3.5、
  roundtrip(π)=3.1415927410125732（单精度舍入可见）、float32 全局变量、
  ptr[float32] 读写。
- **cdecl:sample**：sample.h 的 `float scalef(float,float)` /
  `extern float global_ratiof` 自动生成 float32 并运行正确。
- **反例**：f27（关键字实参，check 拒绝）、s04（unknown 实参，build 拒绝）。
- **回归**：全套件 **227 ran ok**（基线 223 + 新 4）+ fail/strict/cdecl/cfi
  全过；与 base 逐条 diff 仅新增用例，**零回归**。仅有的 2 个 OUTPUT DIFF
  （118/154）在 base f0a4af4 独立 worktree 同样复现（§1.3，Debug STL 断言，
  生成器路径，与 FFI 无关）。
- **ASan**：212–215 与既有 FFI 用例（97/98/101/103/104/107/108/109/110/
  111/112/113）全部 ran ok。
- 规模检查：仍只有原有 5 处超限文件，无新增；codegen_call.cpp 1132<1200。

### 6.3 性能注记（原型未优化）

lazy=1：首次调用多一次 dlsym（含 dlerror 清/查），此后每次调用多
load+test+jne（缓存命中）与间接 call（vs 链接期直 call）——分支预测稳定后
开销很小，但**没有**做 PLT 式惰性绑定优化。lazy=2：每次调用都 dlsym，
热路径应加 (handle,symbol) 缓存（后续）。本轮不做基准，先验走通路径。

## 7. 风险与后续（坑位清单）

深坑记录绕行，按优先级：

1. **回调槽不可重入**（既有）：递归/嵌套回调覆盖全局槽。需逐调用点槽或
   TLS + 闭包生命周期管理。免写路径本轮不接回调，绕开。
2. **cdecl 宏/条件编译**（既有）：函数式宏、#if 分支不展开。真解需
   libclang/完整预处理器；建议保持子集 + 明确警告，另开 cdecl 增强票。
3. **float32 结构体字段 / 含 float 回调**：本轮显式拒绝。字段访问要按
   fwidth=4 走 movss+cvt（gen_field_load/store 目前无浮点分支），跳板要
   单精度提升；中等深度，单独一轮。
4. **免写函数当值**：c_lazy 函数传递/存容器需要「捕获句柄的闭包 + 统一
   约定 shim」，涉及函数值 ABI；本轮拒绝（ Name 当值会落到未绑定诊断）。
5. **dlsym 缓存与句柄生命周期**：lazy=2 逐次 dlsym；句柄无自动 dlclose
   （泄漏到进程退出，ctypes 同语义）；RTLD_GLOBAL/flags 参数未暴露。
6. **Windows**：rt_cffi.c 给 NotImplementedError 桩；正式需
   LoadLibrary/GetProcAddress + Win64 调用约定（影子空间已有基础设施）。
7. **DYLIB 跨模块**：enc_type 按 unknown 序列化，跨模块传句柄会丢类型；
   需要时给 DYLIB 一个稳定编码。
8. **标注覆盖的上下文**：目前只覆盖 AnnAssign 与 return 标注；
   `float(sqrt(2))` 这类转换包装、容器元素标注不传播（推断默认兜底）。
9. **既有 Debug 断言**（118/154，vreg_nonneg OOB）：与 FFI 无关，base 复现，
   建议单独开票（生成器 spill 路径直接 append vreg_ty 未同步 vreg_nonneg）。
10. **安全契约**：免写直调 = 任意符号调用，ABI 不符会**静默算错**（ctypes
    同契约）。文档与错误消息已指明；生产代码建议 cimport 显式声明。

## 8. 集成建议

- 与 CPython 桥（207–211）无冲突：c/dylib 是独立伪模块，测试编号 212+ 已避让；
  本轮未动 CHANGELOG（避免与主树集成冲突），合并时补 0.30x 条目。
- 合并顺序建议：CPython 桥先落（主树在集成），cffi 分支 rebase 后跑全套件；
  关注 driver_build.cpp（kModuleTable/link_program）与 builtin_sigs.cpp 的
  文本冲突面，语义上互不相交。
- 提交只落 cffi 分支，未推送；主树与主树 build 目录零改动。
