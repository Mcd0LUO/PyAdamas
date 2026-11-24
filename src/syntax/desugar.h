// syntax/desugar.h —— 解析后的语法糖展开
#pragma once

#include "common/ast.h"

namespace pyadamas {

// 把 lambda 展开成「合成嵌套 def + 名字引用」，把生成器表达式展开成
// 「合成生成器函数 + 一次调用」，之后类型检查和降级只面对普通函数，
// 闭包捕获/引用计数复用同一条路径。
void desugar_lambdas(Module& m);

}  // namespace pyadamas
