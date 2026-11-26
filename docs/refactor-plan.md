# 重构计划：三个剩余的大文件

> 本文件记录 0.581–0.584 完成 8 处拆分后，剩下三处的**设计方案**与**已验证的障碍**。
> 每次尝试的失败原因都写在下面，避免后来者重复踩。

## 已完成（供对照）

| 文件 | 拆前 | 拆后 | 拆出 | 拆分依据 |
|---|---|---|---|---|
| `typecheck/func.cpp` | 1200 | 486 | `classdef.cpp` | 函数 vs 类 |
| `typecheck/stmt.cpp` | 1200 | 881 | `cstruct.cpp`、`narrow.cpp` | C 结构体建模 / 流窄化 |
| `lower/stmt_for.cpp` | 1176 | 701 | `for_pairs.cpp` | zip/enumerate 成对迭代 |
| `typecheck/expr.cpp` | 1190 | 875 | `operators.cpp` | 运算符类型规则 |
| `lower/call.cpp` | 1149 | 954 | `call_asyncio.cpp` | asyncio 内建调用 |
| `backend/call_rt.cpp` | 1155 | 930 | `call_rc.cpp` | 引用计数内联 |
| `syntax/desugar.cpp` | 1184 | 1012 | `desugar_scan.{h,cpp}` | AST 前置判定 |

共同点：被拆出的部分**不依赖 TU 局部类型**，可以直接搬。剩下三处都不满足这个条件。

## 1. `driver/build.cpp`（1200）

**现状**：一个 TU 里同时有
- 缓存子系统：`Manifest` 结构、`read_manifest`/`write_manifest`、`try_cache`、`cache_root`、
  `set_cache_paths`、`file_digest`、`toolchain_id`、`avx2_emit_tag`、`cc_extra_flags`、
  `write_atomic`、`cache_temp_path`、`parse_u64`；
- 构建管线：`parse_unit`、`resolve_import`、`visit_unit`、`assemble_object`、`build_unit`、
  `link_program`、`load_program`、`prepare_program`。

**障碍**：缓存子系统全部在匿名命名空间里，且 `Manifest`/`parse_u64` 是 TU 局部定义；
管线里的 `build_unit` 直接调用它们。

**方案**（按此顺序做，不要跳步）：
1. 建 `driver/build_cache.h`：把 `struct Manifest` 与上述缓存函数**声明**放进去（都在 `namespace pyadamas`）；
2. 建 `driver/build_cache.cpp`：把这些函数**定义**整体搬过去，**去掉匿名命名空间**（要外部链接）；
3. `build.cpp` 删除这些定义，改 `#include "driver/build_cache.h"`；
4. 管线侧（`build_unit` 等）保持不动，此时 `build.cpp` 预计 ~500 行；
5. 之后再考虑把 `build_unit` 单独拆出（第二步，可选）。

**已踩的坑**：只搬 `build_unit` 而不搬缓存子系统 → `assemble_object`/`try_cache`/`read_manifest`/
`write_atomic` 全部未声明；给它们补内部头又发现 `Program` 类型缺失、匿名命名空间里名字查找失败。
结论：**必须先整体搬缓存子系统**，不能先搬管线。

## 2. `cimport/cdecl_parse.cpp`（1141）

**现状**：`CType`、`StructFieldDef`、`StructDef` 三个类型 + `cpya_type`/`cpya_field_type` 映射函数 +
`parse_struct_body`/`cdecl_process` 主流程挤在一起。

**障碍**：三个类型**互相引用且顺序敏感**（`StructFieldDef` 需要 `CType`，`StructDef` 需要
`StructFieldDef`），且都在匿名命名空间里；`kInt32`/`kUInt32` 是 TU 局部常量，被映射函数引用。

**方案**：
1. `cimport/cdecl_model.h`：按 `CType` → `StructFieldDef` → `StructDef` 顺序定义三个类型，
   常量改 `inline constexpr`，并声明 `cpya_type`/`cpya_field_type`；
2. `cimport/cdecl_types.cpp`：只放这两个映射函数的定义（`namespace pyadamas`，非匿名）；
3. `cdecl_parse.cpp` 删除类型与映射函数定义，保留 token 解析主流程。

**已踩的坑**：三次尝试，最后一次编译通过但 `cdecl:sample` **段错误**——`kInt32` 在头里是
`inline constexpr`、在 .cpp 里还有一份定义，ODR 层面出现两份；另外类型顺序错一次会报
`field 'type' has incomplete type`。结论：**类型顺序 + 常量唯一性必须一次到位**，不能分批改。

## 3. `opt/vect_planner_loop.cpp`（1200）

**现状**：整个文件是**一个 1180 行的 `VectPlanner::try_loop`**，没有函数级缝合线。

**障碍**：这不是「文件太长」，是「函数太长」。文本搬移无从下手。

**方案**：按阶段拆成 `VectPlanner` 的私有方法（同文件内，先不改文件结构）：
1. `analyze_feasibility(...)`：元素类型、步长、边界可向量化的判定；
2. `analyze_dependences(...)`：读写集与跨迭代依赖；
3. `estimate_cost(...)`：向量化 vs 标量的代价比较；
4. `emit_vector_loop(...)`：实际发射。
每拆一步跑一次 `tools/difftest.sh`（向量化直接影响生成代码，必须差分验证）。
拆完后再按需把 3/4 移到独立 TU。

## 顺序建议

1. `cdecl_parse.cpp`（方案最清晰，但必须一次到位）；
2. `driver/build.cpp`（先整体搬缓存子系统）；
3. `vect_planner_loop.cpp`（逻辑重构，风险最高，配 difftest）。
