// lower.h —— AST → IR
#pragma once

#include <string>
#include <vector>

#include "ir.h"
#include "typecheck.h"

namespace py23 {

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
    ModuleLookup module_lookup;
};

// 把已通过类型检查的 AST 降级成 IR。
//
// 只覆盖后端当前支持的子集；遇到尚未实现的构造会记一条错误并尽量
// 继续降级，这样一次能报出全部缺口，而不是改一个跑一次。
LowerResult lower_module(Module& ast, TypeChecker& tc, const std::string& file,
                         const LowerOptions& options = {});

}  // namespace py23
