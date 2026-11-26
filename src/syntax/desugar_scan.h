#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "common/ast.h"

namespace pyadamas {

// 脱糖前置判定：名字绑定扫描 + range 形状识别。只依赖 AST，与改写逻辑无关。
bool target_binds(const Expr* e, const std::string& n);
bool args_bind(const Arguments& a, const std::string& n);
bool stmt_binds(const Stmt& s, const std::string& n);
bool body_binds(const std::vector<StmtPtr>& body, const std::string& n);
bool parse_range(Expr* e, ExprPtr& start, ExprPtr& stop, int64_t& step_val,
                 ExprPtr& step_expr);

}  // namespace pyadamas
