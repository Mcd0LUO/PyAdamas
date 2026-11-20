// codegen.cpp —— IR → x86-64 汇编：数据段、函数遍历与总入口
#include "codegen_impl.h"
#include "codegen.h"
#include "eh_emit.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace pyadamas {

static std::string debug_display_name(const std::string& name0) {
    std::string name = name0;
    auto dollar = name.find('$');
    if (dollar != std::string::npos) name = name.substr(0, dollar);
    auto pos = name.rfind('_');
    if (pos != std::string::npos && pos > 0 && pos + 1 < name.size()) {
        bool digits = true;
        for (size_t i = pos + 1; i < name.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) { digits = false; break; }
        if (digits) return name.substr(0, pos);
    }
    return name;
}

// 与 runtime/rt_dict.c 的字符串哈希保持一致：FNV-1a 64 再 mix64。
// 长串字面量把结果直接写进 PyAdamasStr::hash，字典查字面量键时就不用再算，
// 也不用在 .data.rel.ro（RELRO 后被保护为只读）上做懒缓存。
static uint64_t str_literal_hash(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h ? h : 1;
}

// 数据段直写助手：to_chars + 复用缓冲，免每行 std::format 临时串。
// buf 用 AsmGen::line_buf_（数据段发射不与 line() 嵌套）。
static void quad_dec(MachFunc& out, std::string& buf, int64_t v) {
    buf = "\t.quad ";
    char b[24];
    auto r = std::to_chars(b, b + sizeof b, v);
    buf.append(b, r.ptr);
    buf += '\n';
    out += buf;
}
static void quad_hex(MachFunc& out, std::string& buf, uint64_t v) {
    buf = "\t.quad 0x";
    char b[24];
    auto r = std::to_chars(b, b + sizeof b, v, 16);
    buf.append(b, r.ptr);
    buf += '\n';
    out += buf;
}
static void quad_sym(MachFunc& out, std::string& buf, std::string_view s) {
    buf = "\t.quad ";
    buf += s;
    buf += '\n';
    out += buf;
}
static void append_num(std::string& buf, int64_t v) {
    char t[24];
    auto r = std::to_chars(t, t + sizeof t, v);
    buf.append(t, r.ptr);
}
static void raw_label2(MachFunc& out, std::string& buf, const char* prefix, int64_t a, int64_t b) {
    buf = prefix;
    append_num(buf, a);
    buf += '_';
    append_num(buf, b);
    buf += ":\n";
    out += buf;
}
static void quad_sym2(MachFunc& out, std::string& buf, const char* prefix, int64_t a, int64_t b) {
    buf = "\t.quad ";
    buf += prefix;
    append_num(buf, a);
    buf += '_';
    append_num(buf, b);
    buf += '\n';
    out += buf;
}
static void raw_label(MachFunc& out, std::string& buf, std::string_view name) {
    buf = name;
    buf += ":\n";
    out += buf;
}

// 回调跳板做参数提升时用的寄存器名（SysV 整数参数寄存器）。
static const char* const kCbReg64[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
static const char* const kCbReg32[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
static const char* const kCbReg8[6] = {"%dil", "%sil", "%dl", "%cl", "%r8b", "%r9b"};
// 统一调用约定的整型实参寄存器（与 kCbReg64 相同序列，语义不同：
// 这里表示「第 i 个实参进第 i 个整型寄存器」，与 C ABI 的浮点槽无关。
static const char* const kCbArgReg[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};

std::string AsmGen::gen(const IRModule& m) {
        m_ = &m;
        opt_ = m.opt_level;
        out_.clear();
        eh_plans_.clear();
        eh_plan_active_ = false;
        dbg_on_ = !m.source_path.empty();
        dbg_entries_.clear();
        dbg_seq_ = 0;
        global_labels_.resize(m.globals.size());
        for (size_t i = 0; i < m.globals.size(); ++i)
            global_labels_[i] = m.globals[i].symbol.empty()
                                    ? ".Lg" + std::to_string(i)
                                    : m.globals[i].symbol;
        global_mops_.resize(m.globals.size());
        global_tag_mops_.resize(m.globals.size());
        for (size_t i = 0; i < m.globals.size(); ++i) {
            global_mops_[i] = op_mem_sym(global_labels_[i], "%rip");
            global_tag_mops_[i] = op_mem_sym(global_labels_[i] + "$t", "%rip");
        }

        // P3 门控（设计 §3.4）：模块含 unknown 动态算术/比较才启用 shadow
        // tag。26 个热基准全部不含 pa_any_* → 门控关 → 不进任何 tag 分支。
        // 降级器仍发旧 AnyArith/AnyCmp/AnyNeg，gen_call_rt 在门控内拦截
        // 改发 tag 版（pa_any_*_t）；门控关时逐字旧路径。
        tags_enabled_ = false;
        for (const auto& fp : m.funcs)
            for (const auto& b : fp->blocks)
                for (const auto& in : b.insts)
                    if (in.op == IrOp::CallRt &&
                        (in.imm == static_cast<int64_t>(RtFn::AnyArith) ||
                         in.imm == static_cast<int64_t>(RtFn::AnyCmp) ||
                         in.imm == static_cast<int64_t>(RtFn::AnyNeg)))
                        tags_enabled_ = true;

        if (!m.strings.empty()) {
            // 字符串对象里有一个指向自身字符区的绝对地址。放在 .rodata
            // 会让 PIE 链接器报 "relocation in read-only section"，
            // 必须用可重定位的只读段。
            section_relro();
            for (size_t i = 0; i < m.strings.size(); ++i) {
                const std::string& s = m.strings[i].text;
                out_ += "\t.align 8\n";
                // 隐藏 RC 头（64 字节）：next/prev/rc/dtor/release/size/kind/marked。
                // 字面量 rc = INT64_MIN 永生；启动时只登记进 RC 索引供
                // unknown 类型检查使用，不进 GC 堆链。
                out_ += "\t.quad 0\n\t.quad 0\n";
                out_ += "\t.quad 0x8000000000000000\n";
                out_ += "\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n";
                raw_label(out_, line_buf_, num_label(".Lstr", static_cast<int64_t>(i)));
                // +0 pub：str 是码点数、bytes 是字节数；编译期就算好（RELRO 只读，
                // 运行期不能懒缓存）。+8 字节数，+16 hash（SSO 字符区），
                // +24 data，+32 chash（字典哈希缓存）。
                //
                // **chash 在编译期算好。** 字面量落在 .data.rel.ro，RELRO 之后
                // 是只读的，运行期没法懒缓存 —— 所以必须在这里填。
                // 这样字典查字面量键（if k == "abc" 之类）完全不用算哈希。
                quad_dec(out_, line_buf_, m.strings[i].pub);
                quad_dec(out_, line_buf_, static_cast<int64_t>(s.size()));
                if (s.size() > 7)
                    quad_hex(out_, line_buf_, str_literal_hash(s));
                else
                    out_ += "\t.quad 0\n";
                quad_sym(out_, line_buf_, num_label(".Lstr", static_cast<int64_t>(i)) + "_c");
                quad_hex(out_, line_buf_, str_literal_hash(s));
                raw_label(out_, line_buf_, num_label(".Lstr", static_cast<int64_t>(i)) + "_c");
                line_buf_ = "\t.ascii \"";
                line_buf_ += asm_escape(s);
                line_buf_ += "\"\n";
                out_ += line_buf_;
                out_ += "\t.byte 0\n";
            }
            // 静态字面量虽然永生，但 unknown 值的运行期类型检查要靠 RC
            // 注册表确认载荷类型。表在模块入口统一登记。
        }
        section_relro();
        out_ += "\t.align 8\n.Lstrtab:\n";
        for (size_t i = 0; i < m.strings.size(); ++i)
            quad_sym(out_, line_buf_, num_label(".Lstr", static_cast<int64_t>(i)));
        if (m.strings.empty()) out_ += "\t.quad 0\n";

        if (!m.globals.empty()) {
            out_ += "\t.bss\n";
            out_ += "\t.align 8\n";
            for (size_t i = 0; i < m.globals.size(); ++i) {
                if (m.globals[i].external) continue;
                if (!m.globals[i].symbol.empty())
                    out_ += "\t.globl " + m.globals[i].symbol + "\n";
                line_buf_ = global_label(static_cast<int>(i));
                line_buf_ += ":\n\t.zero 8\n";
                out_ += line_buf_;
                // P3：unknown 全局的平行 tag 字（仅门控内、且别的模块写不到
                // 的全局，见 global_tag_eligible）。初值 0 = UNK 探针回退。
                if (tags_enabled_ && global_tag_eligible(static_cast<int>(i))) {
                    if (!m.globals[i].symbol.empty())
                        out_ += "\t.globl " + global_tag_label(static_cast<int>(i)) + "\n";
                    line_buf_ = global_tag_label(static_cast<int>(i));
                    line_buf_ += ":\n\t.zero 8\n";
                    out_ += line_buf_;
                }
            }

            // GC 根表：所有本模块全局槽的地址；外部全局的存储属于对方模块。
            section_relro();
            out_ += "\t.align 8\n";
            raw_label(out_, line_buf_, roots_label());
            for (size_t i = 0; i < m.globals.size(); ++i) {
                if (m.globals[i].external) continue;
                quad_sym(out_, line_buf_, global_label(static_cast<int>(i)));
            }
        } else {
            section_relro();
            line_buf_ = "\t.align 8\n";
            line_buf_ += roots_label();
            line_buf_ += ":\n";
            out_ += line_buf_;
        }

        // 非入口模块的惰性初始化标志。
        if (!m.is_entry && !m.module_symbol.empty()) {
            out_ += "\t.bss\n\t.align 8\n";
            out_ += std::format("\t.globl {}\n{}:\n\t.zero 8\n",
                                module_ready_symbol(m.module_symbol),
                                module_ready_symbol(m.module_symbol));
        }

        // 每个类一张静态 vtable：槽里是方法代码地址。对象头第一个
        // 字段指向它，虚调用就是「取表、取槽、间接跳转」。
        // vtable 前固定 6 个字的元数据：MRO 元数据指针表、类编号、
        // vtable 自身、本类声明的槽表、模块限定类名、动态属性槽偏移。
        if (!m.classes.empty()) {
            section_relro();
            // 同名类重定义时接口导出的是最后一次绑定，符号也只导出最后一份。
            std::unordered_map<std::string, size_t> last_cls;
            for (size_t i = 0; i < m.classes.size(); ++i) last_cls[m.classes[i].name] = i;
            for (size_t i = 0; i < m.classes.size(); ++i) {
                const IRClass& c = m.classes[i];
                // 导入祖先的 mirror 元数据：cid 固定 -1，只用于限定名比较，
                // vtable/own_slots 指向定义模块导出的表。
                for (size_t j = 0; j < c.ext_bases.size(); ++j) {
                    const IRClassExtBase& e = c.ext_bases[j];
                    out_ += "\t.align 8\n";
                    raw_label2(out_, line_buf_, ".Lvtmetaext", static_cast<int64_t>(i), static_cast<int64_t>(j));
                    out_ += "\t.quad 0\n";
                    out_ += "\t.quad -1\n";
                    quad_sym(out_, line_buf_, e.vtable_symbol);
                    quad_sym(out_, line_buf_, e.own_slots_symbol);
                    quad_sym2(out_, line_buf_, ".Lcnameext", static_cast<int64_t>(i), static_cast<int64_t>(j));
                    out_ += "\t.quad 0\n";  // 导入类没有动态属性槽
                    raw_label2(out_, line_buf_, ".Lcnameext", static_cast<int64_t>(i), static_cast<int64_t>(j));
                    line_buf_ = "\t.asciz \"";
                    line_buf_ += asm_escape(e.qualname);
                    line_buf_ += "\"\n";
                    out_ += line_buf_;
                }
                out_ += "\t.align 8\n";
                raw_label(out_, line_buf_, num_label(".Lvtmeta", static_cast<int64_t>(i)));
                quad_sym(out_, line_buf_, num_label(".Lvtmro", static_cast<int64_t>(i)));
                quad_dec(out_, line_buf_, static_cast<int64_t>(i));
                quad_sym(out_, line_buf_, num_label(".Lvt", static_cast<int64_t>(i)));
                quad_sym(out_, line_buf_, num_label(".Lvtown", static_cast<int64_t>(i)));
                quad_sym(out_, line_buf_, num_label(".Lcname", static_cast<int64_t>(i)));
                // 第 6 个字：动态属性槽偏移（0 = 没有）。
                quad_dec(out_, line_buf_, c.dict_off);
                bool export_cls = !m.is_entry && !m.module_symbol.empty() &&
                                  !c.name.empty() && last_cls[c.name] == i;
                raw_label(out_, line_buf_, num_label(".Lvt", static_cast<int64_t>(i)));
                if (export_cls) {
                    std::string sym = module_class_vtable_symbol(m.module_symbol, c.name);
                    out_ += std::format("\t.globl {}\n\t.hidden {}\n{}:\n", sym, sym, sym);
                }
                if (c.vtable.empty()) out_ += "\t.quad 0\n";
                for (size_t s = 0; s < c.vtable.size(); ++s) {
                    const std::string& ext =
                        s < c.vtable_ext.size() ? c.vtable_ext[s] : std::string();
                    if (!ext.empty()) {
                        quad_sym(out_, line_buf_, ext);
                    } else if (c.vtable[s] < 0) {
                        out_ += "\t.quad 0\n";
                    } else {
                        quad_sym(out_, line_buf_, func_label(c.vtable[s]));
                    }
                }
                // 每个已填槽导出裸代码符号：导入方 vtable 需要函数地址，
                // 不能引用 .LvtN + offset（那是数据槽地址）。
                if (export_cls) {
                    for (size_t s = 0; s < c.vtable.size(); ++s) {
                        std::string impl;
                        const std::string& ext =
                            s < c.vtable_ext.size() ? c.vtable_ext[s] : std::string();
                        if (!ext.empty())
                            impl = ext;
                        else if (c.vtable[s] >= 0)
                            impl = func_label(c.vtable[s]);
                        if (impl.empty()) continue;
                        std::string sym =
                            module_class_method_symbol(m.module_symbol, c.name,
                                                       static_cast<int>(s));
                        out_ += std::format("\t.globl {}\n\t.hidden {}\n\t.set {}, {}\n",
                                            sym, sym, sym, impl);
                    }
                }
                raw_label(out_, line_buf_, num_label(".Lvtmro", static_cast<int64_t>(i)));
                quad_dec(out_, line_buf_, static_cast<int64_t>(c.mro.size()));
                for (int a : c.mro) {
                    if (a >= 0)
                        quad_sym(out_, line_buf_, num_label(".Lvtmeta", a));
                    else
                        quad_sym2(out_, line_buf_, ".Lvtmetaext", static_cast<int64_t>(i), -a - 1);
                }
                raw_label(out_, line_buf_, num_label(".Lvtown", static_cast<int64_t>(i)));
                if (export_cls) {
                    std::string sym = module_class_own_slots_symbol(m.module_symbol, c.name);
                    out_ += std::format("\t.globl {}\n\t.hidden {}\n{}:\n", sym, sym, sym);
                }
                quad_dec(out_, line_buf_, static_cast<int64_t>(c.own_slots.size()));
                for (int s : c.own_slots)
                    quad_dec(out_, line_buf_, s);
                raw_label(out_, line_buf_, num_label(".Lcname", static_cast<int64_t>(i)));
                line_buf_ = "\t.asciz \"";
                line_buf_ += asm_escape(c.qualname);
                line_buf_ += "\"\n";
                out_ += line_buf_;
            }
        }

        // 每个函数一个静态闭包（code + 0 个捕获），用作无捕获函数值。
        // 布局和堆闭包一致：+0 code，+8 n_caps，+16 起是 cell。
        if (!m.funcs.empty()) {
            section_relro();
            for (size_t i = 0; i < m.funcs.size(); ++i) {
                out_ += "\t.align 8\n";
                out_ += "\t.quad 0\n\t.quad 0\n";
                out_ += "\t.quad 0x8000000000000000\n";
                out_ += "\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n";
                raw_label(out_, line_buf_, num_label(".Lclos", static_cast<int64_t>(i)));
                // 间接调用一律走统一约定：+0 指向 shim（签名含浮点的函数），
                // 否则就是函数本体（统一约定与它的 ABI 恰好一致）。
                if (closure_needs_shim(*m.funcs[i]))
                    quad_sym(out_, line_buf_, shim_label(static_cast<int>(i)));
                else
                    quad_sym(out_, line_buf_, func_label(static_cast<int>(i)));
                out_ += "\t.quad 0\n";
            }
        }

        if (dbg_on_) {
            // 运行时 traceback 表引用的源路径与函数名。
            section_rodata();
            out_ += std::format("\t.Ldbfile: .asciz \"{}\"\n", asm_escape(m.source_path));
            for (size_t i = 0; i < m.funcs.size(); ++i) {
                line_buf_ = "\t.Lfnname";
                append_num(line_buf_, static_cast<int64_t>(i));
                line_buf_ += ": .asciz \"";
                line_buf_ += asm_escape(debug_display_name(m.funcs[i]->name));
                line_buf_ += "\"\n";
                out_ += line_buf_;
            }
        }
        out_ += "\t.text\n";
        // 手写跳板（类析构器 / GC 释放 / 生成器帧释放）的实参寄存器与
        // 影子空间也走 ABI：它们由 C 运行时按平台调用约定回调，最内层
        // 实参寄存器在 Win64 是 %rcx 而不是 %rdi。
        const std::string tramp_a0 = arg_reg_i(0);
        const std::string tramp_a1 = arg_reg_i(1);
        const int tramp_shadow = target_abi().shadow_space;
        // DWARF 行号：.file 1 指定源路径，函数体内的 .loc 指向行号。
        if (!m.source_path.empty())
            out_ += std::format("\t.file 1 \"{}\"\n", asm_escape(m.source_path));

        // 每个有堆字段的类生成一个析构器，对象 rc 归零时由运行时回调。
        // 字段按 field_heap 处理：1 = 已知堆指针（直接 dec），2 = unknown
        // （可能装着整数或堆指针，走注册表判断）。字段偏移与布局一致。
        for (size_t i = 0; i < m.classes.size(); ++i) {
            const IRClass& c = m.classes[i];
            bool has_heap = std::ranges::any_of(c.field_heap, [](uint8_t v) { return v != 0; });
            // 有动态属性槽的类即使没有堆字段也要有析构器：槽里的 dict 归它释放。
            if (!has_heap && c.dict_off == 0) continue;
            out_ += "\n# ---- dtor class " + c.name + " ----\n";
            out_ += "\t.align 8\n";
            out_ += std::format(".Ldtor{}:\n", i);
            if (gnu_cfi()) out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbx\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) out_ += "\t.cfi_offset %rbx, -16\n";
            out_ += "\tmovq " + tramp_a0 + ", %rbx\n";
            if (tramp_shadow) out_ += std::format("\tsubq ${}, %rsp\n", tramp_shadow);
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                out_ += std::format("\tmovq {}(%rbx), {}\n", 8 * (f + 1), tramp_a0);
                out_ += c.field_heap[f] == 2 ? "\tcall pa_rc_dec_unknown\n"
                                             : "\tcall pa_rc_dec\n";
            }
            if (c.dict_off != 0) {
                // 动态属性槽：可能还没分配过（NULL），跳过。
                out_ += std::format("\tmovq {}(%rbx), {}\n", c.dict_off, tramp_a0);
                out_ += std::format("\ttestq {}, {}\n\tje 1f\n\tcall pa_rc_dec\n1:\n",
                                    tramp_a0, tramp_a0);
            }
            if (tramp_shadow) out_ += std::format("\taddq ${}, %rsp\n", tramp_shadow);
            out_ += "\tpopq %rbx\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 8\n";
            if (gnu_cfi()) out_ += "\t.cfi_restore %rbx\n";
            out_ += "\tret\n";
            if (gnu_cfi()) out_ += "\t.cfi_endproc\n";

            // GC 清扫用：只把指向可达对象的字段引用减掉，不释放任何东西。
            out_ += "\n# ---- gc release class " + c.name + " ----\n";
            out_ += "\t.align 8\n";
            out_ += std::format(".Lgcrel{}:\n", i);
            if (gnu_cfi()) out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbx\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) out_ += "\t.cfi_offset %rbx, -16\n";
            out_ += "\tmovq " + tramp_a0 + ", %rbx\n";
            if (tramp_shadow) out_ += std::format("\tsubq ${}, %rsp\n", tramp_shadow);
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                out_ += std::format("\tmovq {}(%rbx), {}\n", 8 * (f + 1), tramp_a0);
                out_ += c.field_heap[f] == 2 ? "\tcall pa_rc_dec_unknown\n"
                                             : "\tcall pa_gc_unref_child\n";
            }
            if (c.dict_off != 0) {
                out_ += std::format("\tmovq {}(%rbx), {}\n", c.dict_off, tramp_a0);
                out_ += std::format("\ttestq {}, {}\n\tje 1f\n\tcall pa_gc_unref_child\n1:\n",
                                    tramp_a0, tramp_a0);
            }
            if (tramp_shadow) out_ += std::format("\taddq ${}, %rsp\n", tramp_shadow);
            out_ += "\tpopq %rbx\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 8\n";
            if (gnu_cfi()) out_ += "\t.cfi_restore %rbx\n";
            out_ += "\tret\n";
            if (gnu_cfi()) out_ += "\t.cfi_endproc\n";
        }

        // 生成器帧的槽释放（GC release 与异常逃出共用）：逐槽 dec 并清零。
        // 清零让函数可以重复调用：正常结束时 status==1、槽已释放，直接跳过；
        // 异常逃出后由运行时调用一次，之后 GC 清扫再调到也是空操作。
        // GC 清扫期间 pa_rc_dec 自带「只调可达对象计数」的保护。
        for (size_t i = 0; i < m.generators.size(); ++i) {
            const IRGenerator& g = m.generators[i];
            out_ += std::format("\n# ---- gcrel gen g{} ----\n", i);
            out_ += "\t.align 8\n";
            out_ += std::format(".Lgengcrel{}:\n", i);
            if (gnu_cfi()) out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbx\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) out_ += "\t.cfi_offset %rbx, -16\n";
            out_ += "\tmovq " + tramp_a0 + ", %rbx\n";
            if (tramp_shadow) out_ += std::format("\tsubq ${}, %rsp\n", tramp_shadow);
            // **异常上下文槽必须在 status 检查之前无条件释放。**
            //
            // status==1 表示生成器已正常终止，RC 槽在 gen_finish 里就释放过
            // 了，所以下面整块跳过。但 exc_cur_slot / exc_top_slot 不同：
            // gen_finish 的顺序是 release_slots() 然后 exc_leave() ——
            // exc_leave 把 current_exc 写回槽，那一刻槽里才有值，而
            // release_slots 已经跑完了。于是正常终止的生成器，帧槽里会
            // 留下一个异常对象（或 save 链），而 status==1 又把释放整块跳过，
            // 对象永远不还。
            //
            // 实测（PYADAMAS_EXC_TRACE + PYADAMAS_GRELDBG，119）：
            //   [exc#5] leave  KeyError at 0x...2a8     ← 写进 s1
            //   [grel]  g=0x...250 status=1 s1=0x...070  ← 销毁时 s1 仍有值
            //   [exc#5] LEAK   KeyError                  ← 没人还
            for (size_t s = 0; s < g.slot_heap.size(); ++s) {
                if (g.slot_heap[s] < 3) continue;
                const int off = 8 * (kGenHeader + static_cast<int>(s));
                out_ += std::format("\tleaq {}(%rbx), {}\n", off, tramp_a0);
                out_ += std::format("\tmovq ${}, {}\n",
                                    static_cast<int>(g.slot_heap[s]), tramp_a1);
                out_ += "\tcall pa_gen_slot_release\n";
            }
            out_ += "\tcmpq $1, 8(%rbx)\n";
            out_ += std::format("\tje .Lgengcreldone{}\n", i);
            for (size_t s = 0; s < g.slot_heap.size(); ++s) {
                if (!g.slot_heap[s] || g.slot_heap[s] >= 3) continue;
                const int off = 8 * (kGenHeader + static_cast<int>(s));
                // slot_heap: 1=堆，2=可能装着非堆值（unknown 流过堆槽）。
                // 清零只在 GC 清扫阶段做；trial/restore 阶段运行时只调计数，
                // 不能破坏生成器帧的内容。
                out_ += std::format("\tleaq {}(%rbx), {}\n", off, tramp_a0);
                out_ += std::format("\tmovq ${}, {}\n",
                                    g.slot_heap[s] == 2 ? 2 : 1, tramp_a1);
                out_ += "\tcall pa_gen_slot_release\n";
            }
            out_ += std::format(".Lgengcreldone{}:\n", i);
            if (tramp_shadow) out_ += std::format("\taddq ${}, %rsp\n", tramp_shadow);
            out_ += "\tpopq %rbx\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 8\n";
            if (gnu_cfi()) out_ += "\t.cfi_restore %rbx\n";
            out_ += "\tret\n";
            if (gnu_cfi()) out_ += "\t.cfi_endproc\n";
        }

        for (size_t i = 0; i < m.funcs.size(); ++i) emit_function(*m.funcs[i], static_cast<int>(i));

        // 统一调用约定的 shim：按真实签名把整型寄存器里的实参重排到
        // %xmm / 栈，返回值也按真实签名放回 %rax。间接调用点不需要知道签名。
        for (size_t i = 0; i < m.funcs.size(); ++i)
            if (closure_needs_shim(*m.funcs[i]))
                emit_closure_shim(*m.funcs[i], static_cast<int>(i));

        if (dbg_on_ && !dbg_entries_.empty()) {
            section_relro(); out_ += "\t.align 8\n";
            out_ += ".Ldbgtab:\n\t.quad .Ldbfile\n\t.quad .Ldbg_entries\n";
            out_ += std::format("\t.quad {}\n.Ldbg_entries:\n", dbg_entries_.size());
            for (const auto& e : dbg_entries_) {
                out_ += std::format("\t.quad {}\n", e.label);
                if (e.fi >= 0)
                    out_ += std::format("\t.quad {}\n", dbg_name_label(e.fi));
                else
                    out_ += "\t.quad 0\n";
                out_ += std::format("\t.long {}\n\t.long 0\n", e.line);
            }
            out_ += "\t.text\n";
        }

        // C 回调跳板：C 只传函数地址，不传环境，所以调用点先把 PyAdamas 闭包
        // 登记到每个签名专属的槽里；跳板把闭包放进 %r10（PyAdamas 的 env 约定）
        // 后直接跳到函数体。32 位/布尔参数按声明宽度提升到 64 位。
        if (!m.cb_sigs.empty()) {
            out_ += "\n# ---- callback trampolines ----\n";
            for (size_t i = 0; i < m.cb_sigs.size(); ++i) {
                out_ += std::format("\t.align 8\n.Lcbtramp{}:\n", i);
                // 跳板自成一段 CFI 区域。入口 CFA=rsp+8；下发 call 前会
                // subq $8 补对齐，CFA 相应变成 rsp+16，返回前恢复。
                if (gnu_cfi()) out_ += "\t.cfi_startproc\n";
                out_ += std::format("\tmovq .Lcbslot{}(%rip), %r10\n", i);
                int gi = 0;
                for (CKind k : m.cb_sigs[i].params) {
                    if (k == CKind::Float) continue;  // xmm 参数直接透传
                    if (k == CKind::Int32)
                        out_ += std::format("\tmovslq {}, {}\n", kCbReg32[gi], kCbReg64[gi]);
                    else if (k == CKind::UInt32)
                        out_ += std::format("\tmovl {}, {}\n", kCbReg32[gi], kCbReg32[gi]);
                    else if (k == CKind::Bool)
                        out_ += std::format("\tmovzbl {}, {}\n", kCbReg8[gi], kCbReg32[gi]);
                    ++gi;
                }
                // C 回调走真实 C ABI（浮点在 %xmm），而闭包 +0 指向统一约定入口。
                // 这里把 C 侧实参按位置搬到统一约定的整型寄存器序列，再进 shim；
                // 浮点返回值由 shim 放 %rax，这里搬回 %xmm0 满足 C 侧。
                int ci = 0;
                for (CKind k : m.cb_sigs[i].params) {
                    if (k == CKind::Float)
                        out_ += std::format("\tmovq %xmm{}, {}\n", ci, kCbArgReg[ci]);
                    else
                        out_ += std::format("\tmovq {}, {}\n", kCbReg64[ci], kCbArgReg[ci]);
                    ++ci;
                }
                out_ += "\txorl %eax, %eax\n";
                // **必须先补 8 字节再 call。**
                //
                // 跳板由 C 当普通函数调用：入口 rsp ≡ 8 (mod 16)。它自己要
                // 再发一次 call，按 ABI 要求 call 时 rsp ≡ 0，所以要先 subq $8。
                //
                // 漏掉这 8 字节时，被调方（PyAdamas 函数或 closure shim）在
                // misaligned 栈上执行；gcc 15 的运行时函数用 movaps 溢出 xmm
                // 直接 #GP。实测：112_callbacks.py、113_qsort.py 在 gcc 15 上
                // Illegal instruction。
                out_ += "\tsubq $8, %rsp\n";
                if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 16\n";
                out_ += "\tcall *(%r10)\n";
                out_ += "\taddq $8, %rsp\n";
                if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 8\n";
                if (m.cb_sigs[i].ret == CKind::Float)
                    out_ += "\tmovq %rax, %xmm0\n";
                out_ += "\tret\n";
                if (gnu_cfi()) out_ += "\t.cfi_endproc\n";
            }
            out_ += "\t.bss\n\t.align 8\n";
            for (size_t i = 0; i < m.cb_sigs.size(); ++i)
                out_ += std::format(".Lcbslot{}:\n\t.zero 8\n", i);
            out_ += "\t.text\n";
        }

        // 免写直调（from c import / dylib 句柄调用）：符号名 C 字符串 +
        // 默认句柄的解析缓存槽。名字表按符号去重，槽在首次调用时填。
        if (!m.c_lazy_syms.empty()) {
            section_relro();
            for (size_t i = 0; i < m.c_lazy_syms.size(); ++i)
                out_ += std::format(".Lcsymstr{}:\n\t.asciz \"{}\"\n", i,
                                    asm_escape(m.c_lazy_syms[i]));
            out_ += "\t.bss\n\t.align 8\n";
            for (size_t i = 0; i < m.c_lazy_syms.size(); ++i)
                out_ += std::format(".Lcsym{}:\n\t.zero 8\n", i);
            out_ += "\t.text\n";
        }

        if (!m.is_entry && !m.module_symbol.empty() && m.entry >= 0) {
            out_ += "\t.globl " + module_init_symbol(m.module_symbol) + "\n";
            emit_type_func(module_init_symbol(m.module_symbol));
            out_ += module_init_symbol(m.module_symbol) + ":\n";
            if (gnu_cfi()) out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbp\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) out_ += "\t.cfi_offset %rbp, -16\n";
            out_ += "\tmovq %rsp, %rbp\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_register %rbp\n";
            out_ += std::format("\tleaq .Lstrtab(%rip), {}\n", arg_reg_i(0));
            out_ += std::format("\tmovq ${}, {}\n", m.strings.size(), arg_reg_i(1));
            out_ += "\tcall pa_register_literals\n";
            // 单字节 str 字面量在生成代码里直接引用运行时驻留表
            // pa_char_tab（见 gen_inst 的 ConstStr），所以函数体之前必须
            // 保证这张表已填好。放这里是因为它依赖 pa_disable_unknown_index
            // 的结果（驻留对象要不要登记进 RC 索引）。
            out_ += "\tcall pa_static_strs_init\n";
            if (dbg_on_ && !dbg_entries_.empty())
            out_ += std::format("\tleaq .Ldbgtab(%rip), {}\n\tcall pa_debug_register\n",
                                    arg_reg_i(0));
            out_ += "\tpopq %rbp\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa %rsp, 8\n";
            // 尾跳进 __pa_main：返回地址原样留在栈顶，CFA 规则跟刚进
            // 本函数时一致，展开器在这里看到的仍是调用方的帧。
            out_ += "\tjmp " + func_label(m.entry) + "\n";
            if (gnu_cfi()) out_ += "\t.cfi_endproc\n";
        }

        // 真正的入口：调用降级出来的 __pa_main，返回它的返回值
        if (m.is_entry) {
            out_ += "\t.globl main\n";
            emit_type_func("main");
            out_ += "main:\n";
            if (gnu_cfi()) out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbp\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) out_ += "\t.cfi_offset %rbp, -16\n";
            out_ += "\tmovq %rsp, %rbp\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa_register %rbp\n";
            out_ += std::format("\tsubq ${}, %rsp\n", out_base() + 16);
            out_ += std::format("\tmovq {}, (%rsp)\n", arg_reg_i(0));
            out_ += std::format("\tmovq {}, 8(%rsp)\n", arg_reg_i(1));
            // GC 根表 + 栈顶。栈顶取 main 的 rbp，收集器向下扫描。
            out_ += std::format("\tleaq {}(%rip), {}\n", roots_label(), arg_reg_i(0));
            out_ += std::format("\tmovq ${}, {}\n", local_global_count(), arg_reg_i(1));
            out_ += std::format("\tmovq %rbp, {}\n", arg_reg_i(2));
            out_ += "\tcall pa_gc_init\n";
            out_ += "\tcall pa_rc_stats_init\n";
            if (disable_unknown_index_)
                out_ += "\tcall pa_disable_unknown_index\n";
            else if (index_readonly_)
                out_ += "\tcall pa_set_index_readonly\n";
            out_ += std::format("\tleaq .Lstrtab(%rip), {}\n", arg_reg_i(0));
            out_ += std::format("\tmovq ${}, {}\n", m.strings.size(), arg_reg_i(1));
            out_ += "\tcall pa_register_literals\n";
            out_ += "\tcall pa_static_strs_init\n";
            if (dbg_on_ && !dbg_entries_.empty())
            out_ += std::format("\tleaq .Ldbgtab(%rip), {}\n\tcall pa_debug_register\n",
                                    arg_reg_i(0));
            out_ += std::format("\tmovslq (%rsp), {}\n", arg_reg_i(0));
            out_ += std::format("\tmovq 8(%rsp), {}\n", arg_reg_i(1));
            out_ += "\tcall pa_sys_set_args\n";
            out_ += std::format("\taddq ${}, %rsp\n", out_base() + 16);
            if (m.entry >= 0) out_ += std::format("\tcall {}\n", func_label(m.entry));
            out_ += "\tpopq %rbp\n";
            if (gnu_cfi()) out_ += "\t.cfi_def_cfa %rsp, 8\n";
            out_ += "\tret\n";
            if (gnu_cfi()) out_ += "\t.cfi_endproc\n";
            emit_size_func("main");
        }
        // 零开销异常（Z2-b2）：所有函数的 .gcc_except_table + 每个 TU 一份的
        // DW.ref 间接指针槽。空计划（老路径）一个字节都不发。
        emit_eh_tables();
        emit_gnu_stack_note();

        if (std::getenv("PYADAMAS_TIMING"))
            std::fprintf(stderr, "[timing] codegen alloc=%.1f emit=%.1f ms\n",
                         alloc_ns_ / 1e6, emit_ns_ / 1e6);
        return render(out_);
    }

// ── 零开销异常（Z2-b2，--zero-cost-exc）────────────────────────────────

void AsmGen::prepare_eh_plan(const IRFunction& f, const std::string& fname) {
    eh_begin_labels_.clear();
    eh_end_labels_.clear();
    eh_lp_labels_.clear();
    eh_begin_seq_ = eh_end_seq_ = eh_lp_seq_ = 0;
    eh_plan_active_ = false;
    if (!zero_cost_exc_ || f.eh_regions.empty() || !gnu_cfi()) return;
    // 预扫描顺序 = 块发射顺序 × 块内指令顺序；发射标记指令时按同样的顺序
    // 取标签，一一对应。
    for (const auto& b : f.blocks)
        for (const auto& in : b.insts) {
            if (in.op == IrOp::EhBegin) eh_begin_labels_.push_back(fresh_label("ehB"));
            else if (in.op == IrOp::EhEnd) eh_end_labels_.push_back(fresh_label("ehE"));
            else if (in.op == IrOp::EhLandingPad)
                eh_lp_labels_.push_back(fresh_label("ehLP"));
        }
    // 每个 EhBegin/EhEnd/EhLandingPad 三元组构成一条保护区（嵌套 try 有多条）。
    // 它们共用同一份 LSDA（多条 call-site 记录）与同一个 landing pad 分发入口。
    const size_t n_regions = eh_begin_labels_.size();
    if (n_regions == 0 || eh_end_labels_.size() != n_regions ||
        eh_lp_labels_.size() != n_regions)
        throw std::logic_error("zero-cost EH: begin/end/landing-pad 标记数量不匹配");
    EhPlanRec rec;
    rec.func_label = fname;
    for (size_t k = 0; k < n_regions; ++k)
        rec.ranges.push_back({eh_begin_labels_[k], eh_end_labels_[k], eh_lp_labels_[k]});
    rec.lsda_index = static_cast<int>(eh_plans_.size());
    eh_plans_.push_back(std::move(rec));
    eh_plan_active_ = true;
}

void AsmGen::emit_eh_directives_for_current() {
    if (!eh_plan_active_ || eh_plans_.empty()) return;
    const EhPlanRec& rec = eh_plans_.back();
    EhFuncPlan plan;
    plan.lsda_index = rec.lsda_index;
    plan.func_label = rec.func_label.c_str();
    for (const auto& rg : rec.ranges) {
        EhRange r;
        r.begin_label = rg.begin.c_str();
        r.end_label = rg.end.c_str();
        r.lp_label = rg.lp.c_str();
        r.is_handler = true;
        plan.ranges.push_back(r);
    }
    std::string text;
    emit_eh_func_directives(text, plan);
    emit_raw(text);
}

void AsmGen::emit_eh_tables() {
    if (eh_plans_.empty()) return;
    std::string text;
    for (const auto& rec : eh_plans_) {
        EhFuncPlan plan;
        plan.lsda_index = rec.lsda_index;
        plan.func_label = rec.func_label.c_str();
        for (const auto& rg : rec.ranges) {
            EhRange r;
            r.begin_label = rg.begin.c_str();
            r.end_label = rg.end.c_str();
            r.lp_label = rg.lp.c_str();
            r.is_handler = true;
            plan.ranges.push_back(r);
        }
        emit_eh_lsda(text, plan);
    }
    emit_eh_dwref(text);
    emit_raw(text);
}

std::string generate_asm(const IRModule& m, bool disable_unknown_index,
                         bool zero_cost_exc, bool index_readonly, bool rc_split,
                         bool ra_graph) {
    AsmGen g;
    g.set_disable_unknown_index(disable_unknown_index);
    g.set_index_readonly(index_readonly);
    g.set_zero_cost_exc(zero_cost_exc);
    g.set_rc_split(rc_split);
    g.set_ra_graph(ra_graph);
    return g.gen(m);
}

}  // namespace pyadamas
