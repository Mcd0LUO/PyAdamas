// opt_detail.h —— O2 优化遍之间共享的内部声明
#pragma once

#include "opt.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace py23 {
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
                              const std::vector<char>& recursive);
bool rewrite_direct_calls(IRModule& m, IRFunction& fn,
                          const std::unordered_map<int, int>& gfunc);

bool licm_function(IRFunction& fn, size_t n_globals);
bool version_bounds_function(IRFunction& fn);
// 循环旋转：把条件计算复制到循环体之后，回边直接走 latch，消掉每次
// 迭代的无条件跳转（代码生成会把「跳到下一块」的 jmp 省掉）。
bool rotate_loops_function(IRFunction& fn);

}  // namespace optdetail
}  // namespace py23
