// emit_sink.h —— 发射原语的最小接口（VectPlanner 抽取的第一阶段）
//
// 目的：让向量化器不再直接依赖 AsmGen。实测向量化器 3414 行只用到 18 个 AsmGen
// 成员，其中真正需要「发射/查询」的原语是下面这 7 个（见
// docs/vect-planner-design.md §2 的调用点计数表）。
//
// 这个接口只影响**编译器自身速度**（每发射一条指令一次虚调用），与生成代码的
// 性能无关 —— 热路径是生成的程序，不是编译器。
//
// 第一阶段只抽接口 + 让 AsmGen 实现，**不改任何调用点**。后续阶段才把
// VectPlanner 搬出来并只持有 EmitSink&。
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "mach_instr.h"

namespace pyadamas {

class EmitSink {
public:
    virtual ~EmitSink() = default;

    // ── 发射 ──────────────────────────────────────────────────────
    // 一条结构化指令（两个操作数版本；MReg 版本由 AsmGen 的普通重载承担）。
    virtual void emit2(MOp op, const MOperand& a, const MOperand& b) = 0;
    // 立即数操作。
    virtual void emit_imm(MOp op, int64_t v, const MOperand& b) = 0;
    // 按位模式发立即数（浮点常量、tag 常量等）。
    virtual void emit_imm_hex(MOp op, uint64_t bits, const MOperand& b) = 0;
    // 条件跳转：op 是 Jcc，target 是标签名。
    virtual void emit_jmp(MOp op, std::string_view target) = 0;
    // 溢出跳转等用助记符直接写（向量化器的 jo 系列）。
    virtual void emit_jmp(const char* mn, const std::string& target) = 0;
    // 一行汇编文本（标签/注释/伪指令）。
    virtual void line(std::string_view s) = 0;
    // 标签定义。
    virtual void label(const std::string& s) = 0;

    // ── 查询 ──────────────────────────────────────────────────────
    // vreg 的来源操作数（寄存器或栈槽）。返回引用：调用点直发，不做拷贝。
    virtual const MOperand& src_mop(VReg r) const = 0;
    // 新标签名（tag 用于可读性，如 "vectvec"）。
    virtual std::string fresh_label(const char* tag) = 0;
};

}  // namespace pyadamas
