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

private:
    EmitSink& emit_;
    ValueQuery& val_;
    SlotQuery& slot_;
};

}  // namespace pyadamas
