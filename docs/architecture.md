# 架构

一句话：**七个阶段串成一条单向流水线，每个阶段只认识前一个阶段的产物**。

```
source.py
   │  Lexer（lexer.h，header-only）
   ▼
token 流（token.h）
   │  Parser（parser.cpp，递归下降 + 优先级爬升）
   ▼
AST（ast.h）
   │  Desugar（desugar.cpp）：lambda → 合成嵌套 def
   ▼
AST（已展开）─────────────► TypeChecker（typecheck.cpp）
   │                          │ 只做检查与推断，不改树
   ▼                          ▼
Lowerer（lower.cpp）◄──── 类型查询接口（typecheck.h）
   │
   ▼
IR（ir.h）：函数 → 基本块 → 三态指令
   │  AsmGen（codegen.cpp）
   ▼
GNU as 汇编（AT&T）
   │  cc + runtime/rt.c
   ▼
可执行文件
```

## 阶段契约

每个阶段都有明确的输入、输出和失败方式。跨阶段的东西只有一种：**类型**。

| 阶段 | 输入 | 输出 | 失败方式 |
|---|---|---|---|
| 词法 | 源码字节 | `vector<Token>` | 抛 `LexError` |
| 语法 | token 流 | `Module`（AST 根） | 抛 `ParseError` |
| 脱糖 | AST | AST（lambda 已展开成嵌套 def） | 不失败 |
| 类型检查 | AST | 无（副作用是 `expr_types_` 等映射） | 收集错误，返回 `bool` |
| 降级 | AST + 类型查询接口 | `IRModule` | 收集 `LowerError`，不抛 |
| 代码生成 | `IRModule` | 汇编文本 | 不失败（前置阶段已挡住） |
| 链接 | 汇编 + `rt.o` | 可执行文件 | `cc` 返回非零 |

词法和语法用异常：出错就没法继续，栈回滚最省事。类型检查和降级用**收集列表**：一个文件里通常不止一处错，一次全报比修一个跑一次强。这个不对称是有意的。

### 类型怎么穿过阶段边界

AST 本身不带类型字段（`ast.h` 里没有 `TypePtr`）。类型单独存在 `TypeChecker` 的几张表里：

- `expr_types_`：表达式节点 → 类型
- `func_scopes_` / `func_infos_`：函数定义节点 → 作用域 / 签名
- `classes_`：类名 → 编译期布局（字段表、方法表、vtable 槽位）

降级阶段通过 `TypeChecker` 的查询接口取值：`type_of(Expr*)`、`scope_of(Stmt*)`、`func_info(Stmt*)`、`module_scope()`、`classes()`。这样做的好处是 AST 保持纯语法结构、可以原样 dump；代价是降级器必须持有 `TypeChecker` 的引用，两者生命周期绑定（见 `lower_module` 的签名）。

类布局是**唯一真相**：`ClassInfo` 里的字段顺序和槽位分配既决定 `field.get` 的偏移，也决定 vtable 的下标。降级器只读它，不自己算——两份布局迟早会分叉。`ClassInfo` 与 `Type` 之间用裸指针相接（所有权在 `TypeChecker::all_classes_`），否则字段类型指回类会形成 `shared_ptr` 环。

IR 则是**自带类型**的：每条指令有 `ty`，每个 vreg 和槽位有 `vreg_ty` / `slot_ty`。原因是后端离前端太远，让它回头查 AST 是不可接受的耦合。

## 模块依赖方向

严格单向，没有环：

```
token.h  ←  lexer.h  ←  parser.h/cpp  ←  ast.h
                                          ↑
typecheck.h ──────────────────────────────┘
     ↑
   ir.h  ←  lower.h/cpp
     ↑
codegen.h/cpp
     ↑
  main.cpp（唯一知道所有阶段的文件）
```

只有 `main.cpp` 同时 include 全部阶段，负责把它们串起来、统一报错格式、决定 CLI 模式。任何阶段都不 include `main.cpp`，也不互相跨层调用（`lower` 是唯一一个同时依赖 AST 和类型检查的阶段，这是它存在的意义）。

## 关键取舍

### 只借语法，不背兼容包袱

不做 `import`、不实现 C API、不追求 `sys`/`gc` 这些模块。语义按静态类型的需要重新定义：变量可以标注、标注就是约束、溢出按 int64 回绕。换来的是**没有运行时元数据、没有字典查名字、调用就是一条 `call`**。

### 不生成 C，自己出汇编

生成 C 能让 gcc 帮忙做寄存器分配和优化，但那样这门语言的上限就被 C 的抽象锁死了（异常、尾调用、自定义调用约定都得绕）。自己出汇编，代价是什么都得自己写（栈帧、寄存器分配、符号约定），收益是这些全都可以按语言的需要改，不用求人。

### 不是 SSA，是「可变槽位 + vreg」

理由写在 [ir.md](ir.md#为什么不是-ssa)。一句话：Python 的变量天生可变，先上 phi 会把循环降级搞复杂，而收益要等优化遍才兑现。

### 前端宽、后端严

类型检查的原则是**宁可漏报，不可误杀**：推断不出来就退化成 `unknown`，凡是涉及 `unknown` 的检查一律放行。真正做不了的东西（`try`/`with`、生成器、装饰器实现）前端**照样放行**，留到降级阶段报「code generator 还不支持」。

这样做的直接后果是：`py23 check` 可以当「这文件语法和类型有没有明显毛病」的通用检查器用（它对 3.8 万个真实 `.py` 文件的通过率见 [compatibility.md](compatibility.md)），而 `py23 build` 才是「能不能真编译」的判据。

## 错误处理

三类错误三种形态，错误消息格式统一为 `路径:行:列: error: 消息`，并附源码行和插入符：

```
bad.py:1:7: error: expected parameter name, found ':'
      1 | def f(:
        |       ^
```

| 类别 | 载体 | 是否会继续 | 退出码 |
|---|---|---|---|
| 词法 / 语法 | `LexError` / `ParseError` 异常 | 否，首个错误即停 | 1 |
| 类型 | `TypeChecker::errors` | 是，一轮报完 | 1 |
| 降级 | `LowerResult::errors` | 是，一轮报完 | 1 |
| 运行时 | `py23_panic*` → `exit(1)` | 否 | 1 |

运行时错误故意不带堆栈：现在没有调试信息，编出来的栈回溯只会误导。`py23_panic_div0` / `panic_overflow` 这类专用入口存在的唯一目的，是把 `#DE` 信号换成一句人能看懂的话。

## 数据所有权

- `TypePtr` 是 `shared_ptr<Type>`，所有类型节点由 `TypeChecker::pool_` 持有并复用；同一个 `int` 全局只有一个实例。
- `IRModule` 用 `vector<unique_ptr<IRFunction>>` 持有函数：降级过程中会不断新增函数（嵌套 `def`、模块顶层），`vector<IRFunction>` 扩容会让外部持有的指针失效。
- AST 用 `unique_ptr` 组成树，`Module` 是根，生命周期贯穿整个编译过程。

## 一次编译的完整调用链

`main.cpp` 的 `run_frontend()` 是前端的统一入口，返回 `{mod, tc}`；之后按模式分派：

| 模式 | 走到哪一步 |
|---|---|
| `tokens` | 只跑词法 |
| `ast` | 跑到语法 |
| `types` / `check` | 跑到类型检查 |
| `ir` | 跑到降级 |
| `asm` | 跑到代码生成 |
| `build` / `run` | 再加一次 `cc` |

`run_frontend` 里有一处细节：`check --syntax` 会跳过类型检查。这条绕过路径是给元编程风格的代码准备的——静态推断对 `typing.py` 那种写法本来就无能为力，让人至少能查语法。
