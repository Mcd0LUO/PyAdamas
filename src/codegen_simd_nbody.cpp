// codegen_simd_nbody.cpp —— nbody 浮点循环 SIMD（0.334，PYADAMAS_SIMD 门控）
// 实现分两块：位置更新 map（try_simd_nbody_pos）与 pairwise 力循环
// （try_simd_nbody_pair，独立文件 codegen_simd_nbody_pair.cpp）。
#include "codegen_impl.h"

namespace pyadamas {

bool AsmGen::try_simd_nbody_pos(const Block& b) {
    (void)b;
    return false;  // 待实现（下一增量）
}

bool AsmGen::try_simd_nbody_pair(const Block& b) {
    (void)b;
    return false;  // 待实现（下一增量）
}

}  // namespace pyadamas
