// codegen_vect.h —— 通用循环自动向量化成员与声明（类体片段）
//
// 从 codegen_impl.h 在类内 #include，不单独编译。实现见 codegen_vect.cpp，
// 设计见 docs/simd-vectorization-design.md §11。与特化 matcher（codegen_simd*）
// 互补：特化认基准精确形状，本文件做**通用**自动向量化——基于 IR 结构 +
// 数据引用/别名分析识别任意可数 for 循环的逐元素 map / reduction，不依赖
// 精确指令形状。门控 PYADAMAS_VECT，**默认关**。
//
// 正确性命门：别名/依赖证不出 → 保守跳过（标量 fallback）；浮点 reduction
// 保序提取累加（与标量逐位一致）；整数 reduction 用 paddq 2-lane（模 2^64
// 结合律精确）+ 出口水平求和。向量循环零调用，守卫 + 标量尾保留全部异常语义。

    // 通用循环向量化开关（PYADAMAS_VECT，默认关）。
    bool vect_enabled_ = false;

    // 通用循环向量化入口（codegen_vect.cpp）：gen_block 在特化 matcher 未命中
    // 后调用。识别可数 for + 逐元素 map/reduction，匹配成功发「守卫 + SSE2
    // 向量循环 + jmp cond（标量尾由原机器照常发射）」，返回 true。不匹配返回
    // false，一个字节不发。门控关时第一个判断就返回。
    bool try_vect_loop(const Block& b);
    // 函数级前提（与 simd_func_ok 同口径，但用 vect_enabled_ 门控）。
    bool vect_func_ok() const;
