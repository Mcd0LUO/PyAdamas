// backend/emitter.h —— 机器码发射缓冲区（从 AsmGen 里拆出的第二个协作对象）
//
// AsmGen 原本自己持有输出缓冲 out_（MachFunc）、标签计数器 label_id_、
// 以及 line() 的前导 tab 缓冲 line_buf_。这三样是纯粹的「输出侧状态」：
// 不参与指令选择，只被发射点读写。拆出来之后，emit 相关的状态有了名字，
// 也为后面把 emit 方法整体搬过来留了位置。
//
// 边界与 Frame 一致：只搬**状态**，发射方法（emit0/1/2、line、directive…）
// 暂时留在 AsmGen 读 emit_.*。等它们搬过来时，这个类才真正变成「发射器」。
#pragma once

#include <string>

#include "backend/mach_instr.h"

namespace pyadamas {

class Emitter {
public:
    // 结构化 Machine 层输出：每条发射落成 MItem（Label/Directive/Instr/Comment/Raw）。
    MachFunc out;
    // 标签编号（.Lxxx 的 xxx）。每次 emit_function 归零。
    int label_id = 0;
    // line() 的前导 tab 缓冲：复用容量，避免每行一次分配。
    std::string line_buf;

    // 取下一个标签号。
    int next_label() { return label_id++; }

    void reset() {
        out = MachFunc{};
        label_id = 0;
        line_buf.clear();
    }
};

}  // namespace pyadamas
