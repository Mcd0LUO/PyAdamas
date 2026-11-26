// opt/detail.h —— O2 优化遍之间共享的内部声明
#pragma once

#include "opt/opt.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pyadamas {
namespace optdetail {

struct CVal {
    bool is_float = false;
    int64_t i = 0;
    double f = 0.0;
};

bool bin_is_safe_int(BinKind b);
bool bin_is_safe_float(BinKind b);
void set_const_int(Inst& in, int64_t v);
void set_const_float(Inst& in, double f);
bool inst_is_pure(const Inst& in);
void dce_function(IRFunction& fn);

/* 「这条指令执行时可能在运行期抛异常吗？」——**唯一事实来源**。
 *
 * 四处共用：fold（DCE / 删死值：删掉会静默吞异常）、loop（LICM 外提：
 * 外提会让异常提前/让零次循环也抛）、global_promote（循环出口延迟写回：
 * 异常展开跳过写回 → 被 catch 后读到陈旧全局）、ifconv（then 块被改成
 * 无条件执行：可能多抛）。0.676 前 loop 与 global_promote 各自维护一份，
 * 两处都漏了 float→int Convert，-O2 实测错值；此后只准走这一个入口。
 *
 * fn 用来取操作数静态类型。**fn == nullptr 表示拿不到类型上下文，按保守
 * 口径**（Convert 目标为任何整数种类都算可能抛；多拒绝几个 int→int 转换
 * 只是少一档优化）。有 fn 时精确到 codegen 的真实行为（backend/ops.cpp
 * 的 gen_convert / 除法路径）：
 *   - Convert 目标 Int：源 float / unknown / 取不到类型 → 可能抛
 *     （cvttsd2si 的 INT64_MIN 哨兵检查抛 ValueError / OverflowError）；
 *   - Convert 目标定宽整数（I8..U64）：裸 cvttsd2si + 截断，不抛；
 *   - Convert 目标 Bool / Float / 其它：不抛；
 *   - 调用族、IndexLoad / IndexStore、Setjmp、Div / Mod / FloorDiv → 可能抛。 */
bool opt_may_raise(const Inst& in, const IRFunction* fn);

std::unordered_map<int, int> collect_static_func_globals(const IRModule& m);
std::vector<char> recursive_functions(const IRModule& m,
                                      const std::unordered_map<int, int>& gfunc);
bool inline_calls_in_function(IRModule& m, IRFunction& fn, int fi,
                              const std::unordered_map<int, int>& gfunc,
                              const std::vector<char>& recursive,
                              bool inline_opt = false);
bool rewrite_direct_calls(IRModule& m, IRFunction& fn,
                          const std::unordered_map<int, int>& gfunc);
// INLINE_OPT P3：纯标量自递归单层展开（严格界）。见 opt/inline.cpp。
bool self_unroll_function(IRModule& m, IRFunction& fn, int fi);

bool licm_function(IRFunction& fn, size_t n_globals);
// RC 冗余消除：删掉互相抵消的 temp/temp_take 对。
bool rc_redundant_pairs(IRFunction& fn);
bool rc_unknown_specialize(IRFunction& fn);
// 槽位转发与分支线程化：短路布尔值不再经临时槽往返，直接按比较结果分支。
void forward_slot_loads(IRFunction& fn);
// `d[k] = d.get(k, def) + delta` 融合成一次 pa_dict_bump（省一次哈希+探测）。
bool fuse_dict_bump(IRFunction& fn);
// 简单菱形 if-conversion：`if cond: acc ±= 1`（then 块只有累加、else 空）压成单块，
// 让循环体满足向量化器的「单块直线」要求。见 opt/ifconv.cpp。
bool if_convert_accumulators(IRFunction& fn);
bool fuse_dict_tryget(IRFunction& fn);
void forward_global_loads(IRFunction& fn);
// 循环内全局变量寄存器化：把全局缓存到局部槽、出口写回。入口模块里
// 模块私有的标量全局允许循环体有调用（其它函数/模块仍要求无调用）。
bool promote_globals_in_loops(IRFunction& fn, const IRModule& m);
bool version_bounds_function(IRFunction& fn);
// 整数循环优化（PYADAMAS_LOOP_OPT 门控，默认关）：见 opt/intloop.cpp。
bool thread_jump_blocks(IRFunction& fn);
bool dedup_block_loads(IRFunction& fn);
// 返回每个函数下标是否「容器形状不变」：不改写任何入参/全局列表的长度或
// 元素种类（不 append/setlen/非标量 index.set，不调非形状不变函数）。
std::vector<char> shape_inert_functions(const IRModule& m);
bool version_bounds_affine_function(IRFunction& fn,
                                    const std::vector<char>* shape_inert = nullptr);
// 循环旋转：把条件计算复制到循环体之后，回边直接走 latch，消掉每次
// 迭代的无条件跳转（代码生成会把「跳到下一块」的 jmp 省掉）。
bool rotate_loops_function(IRFunction& fn);
// 跨块公共子表达式消除：循环条件块算过的纯表达式（如 zr*zr）在体块里
// 直接复用，不再重算。需要前驱出口状态一致，因此放在 CFG 定形之后。
bool cse_function(IRFunction& fn);

}  // namespace optdetail
}  // namespace pyadamas
