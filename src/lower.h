// lower.h —— AST → IR
#pragma once

#include <string>
#include <vector>

#include "ir.h"
#include "typecheck.h"

namespace pyadamas {

struct LowerError {
    int line = 1;
    int col = 1;
    std::string msg;
};

struct LowerResult {
    IRModule mod;
    std::vector<LowerError> errors;
};

struct LowerOptions {
    std::string module_symbol;        // 空表示单文件模式（本地 .LgN）
    std::string runtime_name = "__main__";
    std::string source_path;
    bool is_entry = true;
    // 0 基础 / 1 常规优化 / 2 默认（追加循环与控制流小优化）。
    int opt_level = 2;
    // 整数循环优化位掩码（PYADAMAS_LOOP_OPT，默认 0 全关）。见 opt.h。
    int loop_opt = 0;
    // 内联/devirt 深化开关（PYADAMAS_INLINE_OPT，默认 0 关）。见 opt.h。
    bool inline_opt = false;
    // 只读借用槽细化（PYADAMAS_BORROW_OPT，默认 0 关）。见 lower_borrow.cpp。
    bool borrow_opt = false;
    // 允许浮点归约用 SIMD 分组求和（会改变舍入顺序）。
    bool fast_math = false;
    // 零开销异常（Z5，默认开启）：非可挂起函数里的 try 走 LSDA / landing pad
    // 表驱动路径；可挂起函数（生成器/协程）永久保留 setjmp。
    // --no-zero-cost-exc 可回退（关闭时输出与老路径逐字节一致）。
    bool zero_cost_exc = true;
    ModuleLookup module_lookup;
};

// 把已通过类型检查的 AST 降级成 IR。
//
// 只覆盖后端当前支持的子集；遇到尚未实现的构造会记一条错误并尽量
// 继续降级，这样一次能报出全部缺口，而不是改一个跑一次。
LowerResult lower_module(Module& ast, TypeChecker& tc, const std::string& file,
                         const LowerOptions& options = {});

}  // namespace pyadamas
