// ir/verify.cpp —— IR 不变量校验
#include "ir/verify.h"

#include <cstdio>
#include <cstdlib>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace pyadamas {

namespace {

// 收集一条指令读到的所有 vreg（不含 dst）。
template <typename F>
void for_each_operand(const Inst& in, F&& f) {
    if (in.a != kNoReg) f(in.a);
    if (in.b != kNoReg) f(in.b);
    if (in.c != kNoReg) f(in.c);
    for (VReg r : in.args)
        if (r != kNoReg) f(r);
}

// **恢复侧**的 Gen 指令：只有生成器/协程的 resume 函数里才该出现。
bool is_gen_resume_op(IrOp op) {
    switch (op) {
        case IrOp::GenGetState:
        case IrOp::GenClosing:
        case IrOp::GenSent:
        case IrOp::GenYield:
        case IrOp::GenAwait:
        case IrOp::GenGetExc:
        case IrOp::GenTakeExc:
        case IrOp::GenRethrow:
        case IrOp::GenDone:
            return true;
        default:
            return false;
    }
}

}  // namespace

std::vector<std::string> verify_function(const IRFunction& fn) {
    std::vector<std::string> out;
    const size_t nb = fn.blocks.size();
    if (nb == 0) {
        out.push_back(std::format("{}: 函数没有基本块", fn.name));
        return out;
    }

    // ── 1. 槽位属性自洽：owned 的槽必须可能装着堆引用 ──────────────
    const size_t ns = std::min(fn.slot_ty.size(),
                               std::min(fn.slot_owned.size(), fn.slot_unknown.size()));
    for (size_t i = 0; i < ns; ++i) {
        if (!fn.slot_owned[i]) continue;
        const TypePtr& ty = fn.slot_ty[i];
        if (ir_is_heap(ty) || !ty || ty->kind == TyKind::Unknown) continue;
        if (fn.slot_unknown[i]) continue;  // unknown 流过的槽允许
        out.push_back(std::format("{}: 槽 s{} 标了 owned，但类型是 {}（既非堆也不是 unknown）",
                                  fn.name, i, type_name(ty)));
    }

    // ── 2. vreg 定义唯一 ──────────────────────────────────────────
    std::unordered_map<VReg, int> def_count;
    for (size_t bi = 0; bi < nb; ++bi) {
        const auto& b = fn.blocks[bi];
        for (size_t ii = 0; ii < b.insts.size(); ++ii) {
            const Inst& in = b.insts[ii];
            if (in.dst == kNoReg) continue;
            int& c = def_count[in.dst];
            // 生成器/协程**允许**重复定义：跨挂起点的槽位溢出会在每个续跑块
            // 开头把值从堆帧槽重新装回同一个 vreg（见 lower/gen_spill.cpp）。
            // 这不是 bug，是"vreg 在 resume 里被重新赋值"的既定形态。
            if (!fn.is_generator && ++c == 2)
                out.push_back(std::format("{}: vreg %{} 被重复定义（首见于 {} 块 {} 第 {} 条）",
                                          fn.name, in.dst, b.name, bi, ii));
        }
    }

    // ── 3. 用到的 vreg 必须有定义（同一函数内） ────────────────────
    for (size_t bi = 0; bi < nb; ++bi) {
        const auto& b = fn.blocks[bi];
        for (size_t ii = 0; ii < b.insts.size(); ++ii) {
            const Inst& in = b.insts[ii];
            if (is_gen_resume_op(in.op) && !fn.is_generator)
                out.push_back(std::format("{}: 块 {} 第 {} 条用了恢复侧生成器指令 {}，但函数不是生成器",
                                          fn.name, b.name, ii, in.text()));
            for_each_operand(in, [&](VReg r) {
                if (def_count.find(r) == def_count.end())
                    out.push_back(std::format("{}: 块 {} 第 {} 条读取了未定义的 vreg %{}（{}）",
                                              fn.name, b.name, ii, r, in.text()));
            });
        }
    }

    // ── 4. 块结构：终止指令唯一且在末尾；跳转目标在范围内 ──────────
    // 先把所有跳转目标收一遍：空块只有"还有人跳它"才算错 ——
    // 降级会留下不可达的 `unreachable` 占位块，codegen 会跳过它们。
    for (size_t bi = 0; bi < nb; ++bi) {
        const auto& b = fn.blocks[bi];
        // 空块是合法的：降级会留下 `unreachable` 占位块，codegen 按
        // fallthrough 处理它们（有跳转目标也一样）。
        if (b.insts.empty()) continue;
        // 只查**结构性**终止指令（跳转/返回/挂起）出现在块中间 —— 那是块
        // 结构坏了。`call.rt pa_raise(...)` 这类 noreturn 调用出现在块中间
        // 是合法形态（它是 `raise` 的降级，后面可能还跟着不可达的清理
        // 指令），不能当错误。
        for (size_t ii = 0; ii + 1 < b.insts.size(); ++ii) {
            const IrOp op = b.insts[ii].op;
            const bool structural = op == IrOp::Jump || op == IrOp::Branch ||
                                    op == IrOp::Return || op == IrOp::GenYield ||
                                    op == IrOp::GenAwait || op == IrOp::GenRethrow ||
                                    op == IrOp::GenDone;
            if (structural)
                out.push_back(std::format("{}: 块 {} 第 {} 条是终止指令，但它不是块尾（{}）",
                                          fn.name, b.name, ii, b.insts[ii].text()));
        }
        if (!is_terminator(b.insts.back()))
            out.push_back(std::format("{}: 块 {} 结尾不是终止指令（{}）", fn.name, b.name,
                                      b.insts.back().text()));
        for (const Inst& in : b.insts) {
            auto check_target = [&](int32_t t, const char* what) {
                if (t < 0 || static_cast<size_t>(t) >= nb)
                    out.push_back(std::format("{}: 块 {} 的 {} 目标 {} 越界（块数 {}）",
                                              fn.name, b.name, what, t, nb));
            };
            if (in.op == IrOp::Jump) {
                check_target(in.target, "jump");
            } else if (in.op == IrOp::Branch) {
                check_target(in.target, "branch-true");
                check_target(in.target2, "branch-false");
                if (in.target2 < 0)
                    out.push_back(std::format("{}: 块 {} 的条件跳转没有 false 目标",
                                              fn.name, b.name));
            }
        }
    }

    // ── 5. 槽位下标在范围内 ────────────────────────────────────────
    // 越界的槽号在 codegen 里会读到别的槽（静默错值）或越界写（段错误）。
    for (const auto& b : fn.blocks) {
        for (const Inst& in : b.insts) {
            // 按 **op** 判定，而不是 idx.kind：kind 是迁移期留下的元信息，
            // 有些 pass（如全局提升）改写 op 之后不会同步 kind，拿它当权威
            // 会把合法指令判成越界。
            const bool is_slot_op = in.op == IrOp::LoadSlot ||
                                    in.op == IrOp::StoreSlot ||
                                    in.op == IrOp::SlotAddr;
            if (!is_slot_op) continue;
            if (in.idx.value < 0) continue;  // -1 = 该指令没用到 idx
            if (in.idx.value < 0 ||
                static_cast<size_t>(in.idx.value) >= fn.slot_ty.size())
                out.push_back(std::format("{}: 块 {} 引用了越界槽 s{}（槽数 {}）",
                                          fn.name, b.name, in.idx.value,
                                          fn.slot_ty.size()));
        }
    }

    return out;
}

std::vector<std::string> verify_module(const IRModule& m) {
    std::vector<std::string> out;
    for (const auto& fp : m.funcs) {
        if (!fp) continue;
        auto v = verify_function(*fp);
        out.insert(out.end(), v.begin(), v.end());

        // 模块级表下标：越界同样是静默错值（codegen 直接按下标取表项）。
        for (const auto& b : fp->blocks) {
            for (const Inst& in : b.insts) {
                if (in.idx.value < 0) continue;  // -1 = 该指令没用到 idx
                size_t n = 0;
                const char* what = nullptr;
                switch (in.op) {
                    case IrOp::LoadGlobal:
                    case IrOp::StoreGlobal:
                    case IrOp::GlobalAddr:
                        n = m.globals.size(); what = "全局"; break;
                    case IrOp::ConstStr: n = m.strings.size(); what = "字符串池"; break;
                    case IrOp::FuncAddr:
                    case IrOp::FuncCode:
                    case IrOp::CallDirect:
                        n = m.funcs.size(); what = "函数表"; break;
                    case IrOp::ClassAddr: n = m.classes.size(); what = "类表"; break;
                    case IrOp::CallExtern: n = m.externs.size(); what = "外部符号表"; break;
                    // 外部**变量**在另一张表里（externs 是函数）。
                    case IrOp::LoadExtern:
                    case IrOp::StoreExtern:
                        n = m.extern_vars.size(); what = "外部变量表"; break;
                    default: continue;
                }
                if (in.idx.value < 0 || static_cast<size_t>(in.idx.value) >= n)
                    out.push_back(std::format("{}: 块 {} 引用了越界的{}下标 {}（共 {}）",
                                              fp->name, b.name, what, in.idx.value, n));
            }
        }
    }
    return out;
}

int verify_ir_mode() {
    const char* s = std::getenv("PYADAMAS_VERIFY_IR");
    if (!s || !*s) return 0;
    const int v = std::atoi(s);
    return v < 0 ? 0 : (v > 2 ? 2 : v);
}

void verify_module_or_die(const IRModule& m, const char* tag) {
    const int mode = verify_ir_mode();
    if (mode == 0) return;
    const auto bad = verify_module(m);
    if (bad.empty()) return;
    std::fprintf(stderr, "[verify-ir] %s：%zu 条违规\n", tag, bad.size());
    size_t shown = 0;
    for (const auto& s : bad) {
        if (shown++ >= 20) {
            std::fprintf(stderr, "[verify-ir] …（其余 %zu 条省略）\n", bad.size() - 20);
            break;
        }
        std::fprintf(stderr, "[verify-ir] %s\n", s.c_str());
    }
    if (mode >= 2) {
        std::fprintf(stderr, "[verify-ir] 严格模式：中止编译\n");
        std::exit(70);
    }
}

}  // namespace pyadamas
