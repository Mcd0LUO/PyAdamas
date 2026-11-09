// desugar.h —— 解析后的语法糖展开
#pragma once

#include "ast.h"

namespace py23 {

// 把 lambda 展开成「合成嵌套 def + 名字引用」，之后类型检查和降级
// 只面对普通函数，闭包捕获/引用计数复用同一条路径。
void desugar_lambdas(Module& m);

}  // namespace py23
