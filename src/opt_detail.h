// opt_detail.h —— O2 优化遍之间共享的内部声明
#pragma once

#include "opt.h"

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

std::unordered_map<int, int> collect_static_func_globals(const IRModule& m);
std::vector<char> recursive_functions(const IRModule& m,
                                      const std::unordered_map<int, int>& gfunc);
bool inline_calls_in_function(IRModule& m, IRFunction& fn, int fi,
                              const std::unordered_map<int, int>& gfunc,
                              const std::vector<char>& recursive,
                              bool inline_opt = false);
bool rewrite_direct_calls(IRModule& m, IRFunction& fn,
                          const std::unordered_map<int, int>& gfunc);
// INLINE_OPT P3：纯标量自递归单层展开（严格界）。见 opt_inline.cpp。
bool self_unroll_function(IRModule& m, IRFunction& fn, int fi);

bool licm_function(IRFunction& fn, size_t n_globals);
// RC 冗余消除：删掉互相抵消的 temp/temp_take 对。
bool rc_redundant_pairs(IRFunction& fn);
// 槽位转发与分支线程化：短路布尔值不再经临时槽往返，直接按比较结果分支。
void forward_slot_loads(IRFunction& fn);
// `d[k] = d.get(k, def) + delta` 融合成一次 pa_dict_bump（省一次哈希+探测）。
bool fuse_dict_bump(IRFunction& fn);
// 简单菱形 if-conversion：`if cond: acc ±= 1`（then 块只有累加、else 空）压成单块，
// 让循环体满足向量化器的「单块直线」要求。见 opt_ifconv.cpp。
bool if_convert_accumulators(IRFunction& fn);
bool fuse_dict_tryget(IRFunction& fn);
void forward_global_loads(IRFunction& fn);
// 循环内全局变量寄存器化：无调用的循环里把全局缓存到局部槽。
bool promote_globals_in_loops(IRFunction& fn, const std::vector<IRGlobal>& globals);
bool version_bounds_function(IRFunction& fn);
// 整数循环优化（PYADAMAS_LOOP_OPT 门控，默认关）：见 opt_intloop.cpp。
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
