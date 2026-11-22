// vect_planner.h —— 向量化器协作对象（阶段 3 第一块，逐方法搬）
#pragma once

#include <cstdint>

#include "emit_sink.h"
#include "ir.h"
#include "mach_instr.h"
#include "slot_query.h"
#include "value_query.h"

namespace pyadamas {

class VectPlanner {
public:
    VectPlanner(EmitSink& e, ValueQuery& v, SlotQuery& s) : emit_(e), val_(v), slot_(s) {}

    // 除数是不是正的 2 的幂常量；是则写回移位量 sh。
    bool vect_div_pow2(VReg b, int* sh) const;
    // // 与 %（除数为 2 的幂常量）的 2-lane packed 发射：rd 已持有被除数，原地写回。
    void vect_intdiv2(MReg rd, MReg tmp, bool is_mod, int sh);

    // ── 仿射下标的发射（阶段 3 第 3 步：纯发射，无捕获状态）──────────
    // K = coef*8*j：进循环前算一次，循环里每轮 += coef*16。
    void vect_affine_kinit(MReg k, MReg jreg, int64_t coef);
    void vect_affine_step(MReg k, int64_t coef);
    // coef==1 用 movupd 整装；coef>1 逐 lane movsd + unpcklpd（SSE2 无 gather）。
    void vect_affine_load(MReg base, MReg j, MReg k, int64_t coef, int64_t off,
                          MReg dst, MReg scratch);
    // 写出只支持 coef==1（scatter 在角色判定处就拒绝）。
    void vect_affine_store(MReg base, MReg j, int64_t off, MReg src);

    // ── 归纳变量物化（阶段 3 第 4 步）──
    // dst = [j+off, j+off+1]：与 Convert 的 j_src 分支逐字节同一序列。
    void vect_jvec2(MReg jreg, int64_t off, MReg dst, MReg scr);

private:
    EmitSink& emit_;
    ValueQuery& val_;
    SlotQuery& slot_;
};

}  // namespace pyadamas
