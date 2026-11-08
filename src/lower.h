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

// 把已通过类型检查的 AST 降级成 IR。
//
// 只覆盖后端当前支持的子集；遇到尚未实现的构造会记一条错误并尽量
// 继续降级，这样一次能报出全部缺口，而不是改一个跑一次。
LowerResult lower_module(Module& ast, TypeChecker& tc, const std::string& file);

}  // namespace py23
