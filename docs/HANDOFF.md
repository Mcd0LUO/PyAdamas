# 接续 Handoff —— 0.470（2026-11-24 晚）

> 2026-11-24 快照。仓库 `/src/luo-proj/pyadamas`，工作区干净。
> 本轮交付：for 列表迭代免冗余边界检查、for 迭代源持有修复、入口模块私有标量
> 全局跨调用提升；obj_dispatch 对 Codon **3.42× → 2.56×**。
> 本文档覆盖状态、闸门、已交付、剩余任务与纪律；下次交接直接覆盖。

## 0. 会话形态与推送

- 直接在服务器仓库根运行；日常用户是 **celestea**（uid 1003），构建/测试/基准都在本地跑。
- **不要用 root 跑构建/测试**：会在 `build/lead/modcache`、`/tmp` 留下 root 属主文件，
  celestea 之后会撞 `fs.protected_regular`（现象：`Permission denied` / `cannot write mod.s`），
  看起来像编译失败。确需 root 操作后，把 `build/lead`、`build/gfix` chown 回 celestea。
- push 走 root（GitHub 密钥在 /root/.ssh）：

  ```bash
  ssh -i /home/celestea/.ssh/id_ed25519_root_mc -o BatchMode=yes \
    -o UserKnownHostsFile=/home/celestea/.ssh/known_hosts -o ConnectTimeout=8 root@127.0.0.1 \
    'cd /src/luo-proj/pyadamas && git push origin master'
  ```

- git 需 `-c safe.directory=/src/luo-proj/pyadamas`（celestea 全局已配）。

## 1. 环境与构建目录

| 项 | 值 |
|---|---|
| 版本 | **0.470**（CMake 从 CHANGELOG 首条 `## [x.y]` 推导，别手抄） |
| 服务器 | Debian 13，Xeon Gold 6252 @ 2.10GHz，容器 cpuset 48 核 |
| 工具链 | gcc 15.2、cmake 3.31、ninja、python 3.13、go |
| build/lead | 工作构建（编译器 + rt.o/rt_asan.o/rt_debug.o）；**所有闸门/基准用它** |
| build/gfix | checkall 默认的套件/difftest 构建目录（会增量构建到当前源码） |
| build/release | 0.271 语料 oracle，**绝不重建** |
| build/gfix-deque 等 | 历史构建（0.469 时代）。本轮用它做「改动前后 asm 逐字节相同」对照 |

构建：`cmake --build build/lead -j8`（-j32 也可；modules-ts 构建是串行的）。
改了 CHANGELOG 版本号后要重编，版本字符串会进 `deps --json` 与缓存键。

## 2. 闸门与基线

```bash
# 一键全套（构建 + 套件 + difftest + check_size + 12 基准指令数 + SIMD + git + perf）
bash tools/checkall.sh --full --jobs 8
```

- **difftest 并行度必须 ≤8**：默认 nproc（48）时 tarfile 组（361–364）会因超时/资源
  竞争出现假 DIFF（O2 侧 rc=1 + Traceback）；`PYADAMAS_TEST_JOBS=8` 复跑 440/0。
- **短用例基准不可信**：<2ms 的 builtins 用例（isinstance/hasattr/setattr/type/id/
  len/range/abs）多轮中位数可波动 ±20%，`cmp_bench` 会标「快/慢」。判定回归前先
  用 `cmp` 比两侧 `mod.s` 是否逐字节相同（本轮 3 个「慢」标记即是零代码变化的噪声）。
- **ASan 不在 checkall 默认闸门**；动 lowering 所有权 / runtime / RC / GC 时必须单独跑：
  `PYADAMAS_TEST_ASAN=1 PYADAMAS_TEST_JOBS=8 bash tests/run_tests.sh build/lead build/lead/pyadamas`
- 单用例对拍 CPython：先 `python3 tests/run/NNN.py > /tmp/ref.txt`，再
  `PYADAMAS_CACHE_DIR=/tmp/xx build/lead/pyadamas build --rebuild -o /tmp/x tests/run/NNN.py`，
  diff 输出；新用例的 `# --- expect ---` 段必须来自 CPython 实跑。
- 绑核计时：`taskset -c 24`，多轮取中位数/最小值；机器双峰，别用单轮。
- Codon 九例对比脚本在 `/tmp/pa-codon-cmp/codon_cmp.py`（/tmp 可能被清）：
  9 用例、9 轮随机配对、`taskset -c 24`、输出对 CPython；Codon 二进制在
  `.ref_proj/tools/codon-0.20.7/bin/codon`，obj_dispatch 的等价改写是
  `.ref_proj/benchmarks/obj_dispatch_codon.py`。

本轮实测（0.470）：

| 项 | 结果 |
|---|---|
| 普通套件 | **469 ran ok / 0 失败**，EXIT=0 |
| ASan 套件 | **469 ran ok / 0 失败**，EXIT=0 |
| difftest | **440 / 0**（8 路） |
| O0/O1/O2 一致性 | 全过（tests/opt_levels.sh） |
| check_size | 仅 HEAD 既有两个超限（typecheck/typecheck.cpp 1227、cimport/cdecl_parse.cpp 1271） |
| 全量内置基准 53 项 | 几何 0.979×（3 个 <1.2ms 噪声，asm 逐字节相同） |
| Codon 九例 | 几何 PA/Codon 0.869；obj_dispatch 292.8 / 114.2 ms = 2.56× |

## 3. 本轮交付（0.470）

| commit | 内容 |
|---|---|
| `c95a320` | fix(lower)：for 迭代源在体内被重绑时读到已释放对象；新增 564 |
| `83229e5` | perf(lower)：for 列表/元组迭代免冗余边界检查 |
| `8557f7a` | perf(opt)：入口模块私有标量全局跨调用提升（含两处后端融合）；新增 565 |
| 文档提交 | CHANGELOG 0.470、HANDOFF、ir/regalloc/roadmap/index 同步 |

obj_dispatch 演进：389.9 →（边界检查）302.4 →（迭代源持有）314.3 →（私有全局
提升 + 融合修补）292.5 ms；对 Codon 3.42× → 2.56×。

## 4. 关键设计与坑（改动前必读）

### 4.1 for 迭代源持有（`src/lower/stmt_for.cpp` `lower_for_seq`）

序列表达式若是借用（名字/属性/下标读），循环开始前 `rc_inc + push_temp`，随语句
边界 `rc_cleanup` 释放；`fresh_vregs` 里的新分配值已经由临时栈持有，不再补。
**不要**改成“存进帧槽”：槽会被 RA 复用寄存器，且生命周期会被拉长到函数出口。
参数来源的调用方本来就持引用，多一对 inc/dec 是无害的。

### 4.2 入口私有标量全局提升（`src/opt/global_promote.cpp`）

`entry_private_globals(m)`：入口模块（`__pa_main`）里，除入口外没有任何函数
`LoadGlobal/StoreGlobal`、全模块没有 `GlobalAddr` 的标量全局才算私有。依据：
- 入口符号前缀固定 `__entry__`（`driver/build.cpp`），`import __entry__` 被拒绝；
- `globals/eval/exec` 在代码生成阶段直接报错（无法动态读全局）；
- 入口函数无 handler（`has_handlers`/EH 标记检查），未捕获异常 = 进程终止；
- 被调函数（方法/闭包/生成器/线程）若引用全局，扫描会把它标成非私有。

只有入口函数放宽调用门禁；非入口模块可被 import，保持「循环体无 call、无可抛异常
指令」。堆全局不提升。**新增观察者通路时（C 回调、新 dynamic builtin、跨模块符号
访问）必须回来复查这个证明。**

### 4.3 两处必须同时保住的融合（`vcall_inline.cpp` / `fuse.cpp`）

把 `total` 从 global 改成 slot 后，两个后端优化匹配不上，提升反而慢 7%：
1. `VCall + add + store` 融合原来只认 `store.global`（结果留 %rax）；现扩展为
   `store.slot`，走 `can_fuse` 同套判据，split 模式跳过；“快照位置”判据从只拒
   `%rax` 收紧为拒全部 caller-saved（槽提升后快照是 callee-saved 槽寄存器）。
2. `try_forward_global_across_rc` 的 `across_scalar` 原来只认中间的 `LoadGlobal`，
   提升后是 `LoadSlot`（标量定值）——补上后恢复 dec-forwarding，省每轮一次全局重载。
**教训**：任何“改操作数形态”的 pass 都要对比内层循环 asm，不能只看语义正确。

## 5. 剩余方向（obj_dispatch 仍是最差用例）

剩余开销结构（内层循环 ~292ms / 64M 轮 ≈ 11 周期）：每轮 ~18 条 RC inc/dec
（元素装载 inc + 旧值 dec）、~10 条虚调用守卫（3 vtable 比较）、索引/循环控制。
1. **RC 借用扩展**（最大杠杆，~15%）：把 borrow 扩到“入口全局根 + append-only 根”。
   注意全局槽是 GC 根，不能留悬垂借用；推荐先把全局降级成帧槽再借用，并保留
   fail-closed 的所有权分析。改动触碰 UAF 红线，必须 ASan + 语料对拍 + CPython 对拍。
2. **虚调用守卫**：把 vtable 地址比较换成函数指针比较可省 1–2 条，但会合并
   “同一实现被多类继承”的候选，改变按类匹配的安全论证，谨慎。
3. **RA_OPT 默认化**（`PYADAMAS_RA_OPT=1`）：私有全局提升后 obj_dispatch 只剩 ~2%
   收益；历史上 mandelbrot/fasta +3~4%。默认关，需全量 abbench 再决定。
4. spectral_norm / fannkuch / fasta 的顶层热循环现在应能吃到入口私有提升，本轮
   只测了 obj_dispatch 与九例，建议用 run_bench 对应用例确认收益。
5. 既有杂项：check_size 两个超限文件待拆；`tests/check_size.sh` 的 ALLOW_FILES 为空。

## 6. 教训清单（本轮新增，比数字更值钱）

1. **优化会被下游融合吃掉**：提升把 global 改 slot 后，`VCall+add+store` 与跨 RC
   转发同时失效，先回退 7%；补融合后才净赚。改 IR 形态必须逐条核对消费该形态的
   后端优化（grep op 名）。
2. **ASan 不保证抓住自定义池的 UAF**：for 迭代源悬垂读在 ASan 下 0 报错
   （分配器池化/复用），是 CPython 对拍 + 错误输出暴露的。所有权改动必须配语义对拍。
3. **私有性可以来自符号命名空间**：`__entry__` 不可 import 是 driver 层保证，
   比“模块内没人引用”更强；但 GlobalAddr 逃逸必须单独挡。
4. **短用例基准先 cmp asm 再谈回归**：<2ms 用例 ±20% 是常态。
5. **difftest 别用默认并行度**：48 路会出假 DIFF（超时/竞争），8 路稳定。
6. **“提升/降级 pass 的收益”要按真实热循环验证**：0.269 的全局提升微基准 −84%、
   真实基准 0 收益；0.470 的私有性放宽才让它第一次在真实用例上命中。

## 7. 待决策

- 是否继续压 obj_dispatch 的 RC（borrow 扩展，风险高、需大量验证）？
- 是否把 `PYADAMAS_RA_OPT` 默认打开（需全量 abbench 复核）？
- check_size 两个超限文件何时拆（纯工程债，不阻塞功能）。
