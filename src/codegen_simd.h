// codegen_simd.h —— SIMD 向量化成员与声明（类体片段）
//
// 从 codegen_impl.h 拆出（文件规模闸门 1200 行）。本文件是 class AsmGen
// 的类体片段：由 codegen_impl.h 在类内 #include，不单独编译。
// 实现见 codegen_simd.cpp，设计见 docs/simd-vectorization-design.md。
// 开关 set_simd 留在 codegen_impl.h 的 public 段（generate_asm 外部调用）。

    // 浮点循环 SIMD 向量化（PYADAMAS_SIMD，默认关）。开启后 gen_block
    // 对匹配到的 reduction 热点循环发 SSE2 packed 向量体 + 标量尾循环。
    bool simd_enabled_ = false;

    // SIMD 向量化（codegen_simd.cpp，PYADAMAS_SIMD 门控）：识别 reduction
    // 热点循环体块，匹配成功则发「守卫 + SSE2 向量循环 + 跳标量尾」前奏，
    // 返回 true（随后 gen_block 照常发射原标量指令作为尾循环/fallback）。
    // 不匹配返回 false，一个字节不发。
    bool try_simd_reduction(const Block& b);
