// codegen.cpp —— IR → x86-64 汇编：数据段、函数遍历与总入口
#include "codegen_impl.h"
#include "codegen.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

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

// 回调跳板做参数提升时用的寄存器名（SysV 整数参数寄存器）。
static const char* const kCbReg64[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
static const char* const kCbReg32[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
static const char* const kCbReg8[6] = {"%dil", "%sil", "%dl", "%cl", "%r8b", "%r9b"};

std::string AsmGen::gen(const IRModule& m) {
        m_ = &m;
        opt_ = m.opt_level;
        out_.clear();
        dbg_on_ = !m.source_path.empty();
        dbg_entries_.clear();
        dbg_seq_ = 0;
        global_labels_.resize(m.globals.size());
        for (size_t i = 0; i < m.globals.size(); ++i)
            global_labels_[i] = m.globals[i].symbol.empty()
                                    ? ".Lg" + std::to_string(i)
                                    : m.globals[i].symbol;

        if (!m.strings.empty()) {
            // 字符串对象里有一个指向自身字符区的绝对地址。放在 .rodata
            // 会让 PIE 链接器报 "relocation in read-only section"，
            // 必须用可重定位的只读段。
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            for (size_t i = 0; i < m.strings.size(); ++i) {
                const std::string& s = m.strings[i];
                out_ += "\t.align 8\n";
                // 隐藏 RC 头（64 字节）：next/prev/rc/dtor/release/size/kind/marked。
                // 字面量 rc = INT64_MIN 永生；启动时只登记进 RC 索引供
                // unknown 类型检查使用，不进 GC 堆链。
                out_ += "\t.quad 0\n\t.quad 0\n";
                out_ += "\t.quad 0x8000000000000000\n";
                out_ += "\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n";
                out_ += std::format(".Lstr{}:\n", i);
                out_ += std::format("\t.quad {}\n", s.size());
                if (s.size() > 7)
                    out_ += std::format("\t.quad 0x{:x}\n", str_literal_hash(s));
                else
                    out_ += "\t.quad 0\n";
                out_ += std::format("\t.quad .Lstr{}_c\n", i);
                out_ += std::format(".Lstr{}_c:\n", i);
                out_ += std::format("\t.ascii \"{}\"\n", asm_escape(s));
                out_ += "\t.byte 0\n";
            }
            // 静态字面量虽然永生，但 unknown 值的运行期类型检查要靠 RC
            // 注册表确认载荷类型。表在模块入口统一登记。
        }
        out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
        out_ += "\t.align 8\n.Lstrtab:\n";
        for (size_t i = 0; i < m.strings.size(); ++i)
            out_ += std::format("\t.quad .Lstr{}\n", i);
        if (m.strings.empty()) out_ += "\t.quad 0\n";

        if (!m.globals.empty()) {
            out_ += "\t.bss\n";
            out_ += "\t.align 8\n";
            for (size_t i = 0; i < m.globals.size(); ++i) {
                if (m.globals[i].external) continue;
                if (!m.globals[i].symbol.empty())
                    out_ += "\t.globl " + m.globals[i].symbol + "\n";
                out_ += std::format("{}:\n\t.zero 8\n", global_label(static_cast<int>(i)));
            }

            // GC 根表：所有本模块全局槽的地址；外部全局的存储属于对方模块。
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            out_ += "\t.align 8\n";
            out_ += std::format("{}:\n", roots_label());
            for (size_t i = 0; i < m.globals.size(); ++i) {
                if (m.globals[i].external) continue;
                out_ += std::format("\t.quad {}\n", global_label(static_cast<int>(i)));
            }
        } else {
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            out_ += std::format("\t.align 8\n{}:\n", roots_label());
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
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
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
                    out_ += std::format(".Lvtmetaext{}_{}:\n", i, j);
                    out_ += "\t.quad 0\n";
                    out_ += "\t.quad -1\n";
                    out_ += std::format("\t.quad {}\n", e.vtable_symbol);
                    out_ += std::format("\t.quad {}\n", e.own_slots_symbol);
                    out_ += std::format("\t.quad .Lcnameext{}_{}\n", i, j);
                    out_ += "\t.quad 0\n";  // 导入类没有动态属性槽
                    out_ += std::format(".Lcnameext{}_{}:\n", i, j);
                    out_ += std::format("\t.asciz \"{}\"\n", asm_escape(e.qualname));
                }
                out_ += "\t.align 8\n";
                out_ += std::format(".Lvtmeta{}:\n", i);
                out_ += std::format("\t.quad .Lvtmro{}\n", i);
                out_ += std::format("\t.quad {}\n", i);
                out_ += std::format("\t.quad .Lvt{}\n", i);
                out_ += std::format("\t.quad .Lvtown{}\n", i);
                out_ += std::format("\t.quad .Lcname{}\n", i);
                // 第 6 个字：动态属性槽偏移（0 = 没有）。
                out_ += std::format("\t.quad {}\n", c.dict_off);
                bool export_cls = !m.is_entry && !m.module_symbol.empty() &&
                                  !c.name.empty() && last_cls[c.name] == i;
                out_ += std::format(".Lvt{}:\n", i);
                if (export_cls) {
                    std::string sym = module_class_vtable_symbol(m.module_symbol, c.name);
                    out_ += std::format("\t.globl {}\n\t.hidden {}\n{}:\n", sym, sym, sym);
                }
                if (c.vtable.empty()) out_ += "\t.quad 0\n";
                for (size_t s = 0; s < c.vtable.size(); ++s) {
                    const std::string& ext =
                        s < c.vtable_ext.size() ? c.vtable_ext[s] : std::string();
                    if (!ext.empty()) {
                        out_ += std::format("\t.quad {}\n", ext);
                    } else if (c.vtable[s] < 0) {
                        out_ += "\t.quad 0\n";
                    } else {
                        out_ += std::format("\t.quad {}\n", func_label(c.vtable[s]));
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
                out_ += std::format(".Lvtmro{}:\n", i);
                out_ += std::format("\t.quad {}\n", c.mro.size());
                for (int a : c.mro) {
                    if (a >= 0)
                        out_ += std::format("\t.quad .Lvtmeta{}\n", a);
                    else
                        out_ += std::format("\t.quad .Lvtmetaext{}_{}\n", i, -a - 1);
                }
                out_ += std::format(".Lvtown{}:\n", i);
                if (export_cls) {
                    std::string sym = module_class_own_slots_symbol(m.module_symbol, c.name);
                    out_ += std::format("\t.globl {}\n\t.hidden {}\n{}:\n", sym, sym, sym);
                }
                out_ += std::format("\t.quad {}\n", c.own_slots.size());
                for (int s : c.own_slots)
                    out_ += std::format("\t.quad {}\n", s);
                out_ += std::format(".Lcname{}:\n", i);
                out_ += std::format("\t.asciz \"{}\"\n", asm_escape(c.qualname));
            }
        }

        // 每个函数一个静态闭包（code + 0 个捕获），用作无捕获函数值。
        // 布局和堆闭包一致：+0 code，+8 n_caps，+16 起是 cell。
        if (!m.funcs.empty()) {
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            for (size_t i = 0; i < m.funcs.size(); ++i) {
                out_ += "\t.align 8\n";
                out_ += "\t.quad 0\n\t.quad 0\n";
                out_ += "\t.quad 0x8000000000000000\n";
                out_ += "\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n";
                out_ += std::format(".Lclos{}:\n", i);
                out_ += std::format("\t.quad {}\n", func_label(static_cast<int>(i)));
                out_ += "\t.quad 0\n";
            }
        }

        if (dbg_on_) {
            // 运行时 traceback 表引用的源路径与函数名。
            out_ += "\t.section .rodata\n";
            out_ += std::format("\t.Ldbfile: .asciz \"{}\"\n", asm_escape(m.source_path));
            for (size_t i = 0; i < m.funcs.size(); ++i)
                out_ += std::format("\t.Lfnname{}: .asciz \"{}\"\n", i,
                                    asm_escape(debug_display_name(m.funcs[i]->name)));
        }
        out_ += "\t.text\n";
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
            out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbx\n";
            out_ += "\t.cfi_def_cfa_offset 16\n";
            out_ += "\t.cfi_offset %rbx, -16\n";
            out_ += "\tmovq %rdi, %rbx\n";
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                out_ += std::format("\tmovq {}(%rbx), %rdi\n", 8 * (f + 1));
                out_ += c.field_heap[f] == 2 ? "\tcall pa_rc_dec_unknown\n"
                                             : "\tcall pa_rc_dec\n";
            }
            if (c.dict_off != 0) {
                // 动态属性槽：可能还没分配过（NULL），跳过。
                out_ += std::format("\tmovq {}(%rbx), %rdi\n", c.dict_off);
                out_ += "\ttestq %rdi, %rdi\n\tje 1f\n\tcall pa_rc_dec\n1:\n";
            }
            out_ += "\tpopq %rbx\n";
            out_ += "\t.cfi_def_cfa_offset 8\n";
            out_ += "\t.cfi_restore %rbx\n";
            out_ += "\tret\n";
            out_ += "\t.cfi_endproc\n";

            // GC 清扫用：只把指向可达对象的字段引用减掉，不释放任何东西。
            out_ += "\n# ---- gc release class " + c.name + " ----\n";
            out_ += "\t.align 8\n";
            out_ += std::format(".Lgcrel{}:\n", i);
            out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbx\n";
            out_ += "\t.cfi_def_cfa_offset 16\n";
            out_ += "\t.cfi_offset %rbx, -16\n";
            out_ += "\tmovq %rdi, %rbx\n";
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                out_ += std::format("\tmovq {}(%rbx), %rdi\n", 8 * (f + 1));
                out_ += c.field_heap[f] == 2 ? "\tcall pa_rc_dec_unknown\n"
                                             : "\tcall pa_gc_unref_child\n";
            }
            if (c.dict_off != 0) {
                out_ += std::format("\tmovq {}(%rbx), %rdi\n", c.dict_off);
                out_ += "\ttestq %rdi, %rdi\n\tje 1f\n\tcall pa_gc_unref_child\n1:\n";
            }
            out_ += "\tpopq %rbx\n";
            out_ += "\t.cfi_def_cfa_offset 8\n";
            out_ += "\t.cfi_restore %rbx\n";
            out_ += "\tret\n";
            out_ += "\t.cfi_endproc\n";
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
            out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbx\n";
            out_ += "\t.cfi_def_cfa_offset 16\n";
            out_ += "\t.cfi_offset %rbx, -16\n";
            out_ += "\tmovq %rdi, %rbx\n";
            out_ += "\tcmpq $1, 8(%rbx)\n";
            out_ += std::format("\tje .Lgengcreldone{}\n", i);
            for (size_t s = 0; s < g.slot_heap.size(); ++s) {
                if (!g.slot_heap[s]) continue;
                const int off = 8 * (kGenHeader + static_cast<int>(s));
                // slot_heap: 1=堆，2=可能装着非堆值（unknown 流过堆槽）。
                // 清零只在 GC 清扫阶段做；trial/restore 阶段运行时只调计数，
                // 不能破坏生成器帧的内容。
                out_ += std::format("\tleaq {}(%rbx), %rdi\n", off);
                out_ += std::format("\tmovq ${}, %rsi\n",
                                    g.slot_heap[s] == 2 ? 2 : 1);
                out_ += "\tcall pa_gen_slot_release\n";
            }
            out_ += std::format(".Lgengcreldone{}:\n", i);
            out_ += "\tpopq %rbx\n";
            out_ += "\t.cfi_def_cfa_offset 8\n";
            out_ += "\t.cfi_restore %rbx\n";
            out_ += "\tret\n";
            out_ += "\t.cfi_endproc\n";
        }

        for (size_t i = 0; i < m.funcs.size(); ++i) emit_function(*m.funcs[i], static_cast<int>(i));

        if (dbg_on_ && !dbg_entries_.empty()) {
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n\t.align 8\n";
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
                // 跳板自成一段 CFI 区域：它不动 rsp，C 的返回地址就在
                // 栈顶，展开时直接按入口规则（CFA=rsp+8）继续往上走。
                out_ += "\t.cfi_startproc\n";
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
                out_ += "\tjmp *(%r10)\n";
                out_ += "\t.cfi_endproc\n";
            }
            out_ += "\t.bss\n\t.align 8\n";
            for (size_t i = 0; i < m.cb_sigs.size(); ++i)
                out_ += std::format(".Lcbslot{}:\n\t.zero 8\n", i);
            out_ += "\t.text\n";
        }

        if (!m.is_entry && !m.module_symbol.empty() && m.entry >= 0) {
            out_ += "\t.globl " + module_init_symbol(m.module_symbol) + "\n";
            out_ += "\t.type " + module_init_symbol(m.module_symbol) + ", @function\n";
            out_ += module_init_symbol(m.module_symbol) + ":\n";
            out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbp\n";
            out_ += "\t.cfi_def_cfa_offset 16\n";
            out_ += "\t.cfi_offset %rbp, -16\n";
            out_ += "\tmovq %rsp, %rbp\n";
            out_ += "\t.cfi_def_cfa_register %rbp\n";
            out_ += "\tleaq .Lstrtab(%rip), %rdi\n";
            out_ += std::format("\tmovq ${}, %rsi\n", m.strings.size());
            out_ += "\tcall pa_register_literals\n";
            if (dbg_on_ && !dbg_entries_.empty())
                out_ += "\tleaq .Ldbgtab(%rip), %rdi\n\tcall pa_debug_register\n";
            out_ += "\tpopq %rbp\n";
            out_ += "\t.cfi_def_cfa %rsp, 8\n";
            // 尾跳进 __pa_main：返回地址原样留在栈顶，CFA 规则跟刚进
            // 本函数时一致，展开器在这里看到的仍是调用方的帧。
            out_ += "\tjmp " + func_label(m.entry) + "\n";
            out_ += "\t.cfi_endproc\n";
        }

        // 真正的入口：调用降级出来的 __pa_main，返回它的返回值
        if (m.is_entry) {
            out_ += "\t.globl main\n";
            out_ += "\t.type main, @function\n";
            out_ += "main:\n";
            out_ += "\t.cfi_startproc\n";
            out_ += "\tpushq %rbp\n";
            out_ += "\t.cfi_def_cfa_offset 16\n";
            out_ += "\t.cfi_offset %rbp, -16\n";
            out_ += "\tmovq %rsp, %rbp\n";
            out_ += "\t.cfi_def_cfa_register %rbp\n";
            out_ += "\tsubq $16, %rsp\n";
            out_ += "\tmovq %rdi, (%rsp)\n";
            out_ += "\tmovq %rsi, 8(%rsp)\n";
            // GC 根表 + 栈顶。栈顶取 main 的 rbp，收集器向下扫描。
            out_ += std::format("\tleaq {}(%rip), %rdi\n", roots_label());
            out_ += std::format("\tmovq ${}, %rsi\n", local_global_count());
            out_ += "\tmovq %rbp, %rdx\n";
            out_ += "\tcall pa_gc_init\n";
            out_ += "\tcall pa_rc_stats_init\n";
            if (disable_unknown_index_)
                out_ += "\tcall pa_disable_unknown_index\n";
            out_ += "\tleaq .Lstrtab(%rip), %rdi\n";
            out_ += std::format("\tmovq ${}, %rsi\n", m.strings.size());
            out_ += "\tcall pa_register_literals\n";
            if (dbg_on_ && !dbg_entries_.empty())
                out_ += "\tleaq .Ldbgtab(%rip), %rdi\n\tcall pa_debug_register\n";
            out_ += "\tmovslq (%rsp), %rdi\n";
            out_ += "\tmovq 8(%rsp), %rsi\n";
            out_ += "\tcall pa_sys_set_args\n";
            out_ += "\taddq $16, %rsp\n";
            if (m.entry >= 0) out_ += std::format("\tcall {}\n", func_label(m.entry));
            out_ += "\tpopq %rbp\n";
            out_ += "\t.cfi_def_cfa %rsp, 8\n";
            out_ += "\tret\n";
            out_ += "\t.cfi_endproc\n";
            out_ += "\t.size main, .-main\n";
        }
        out_ += "\t.section .note.GNU-stack,\"\",@progbits\n";

        if (std::getenv("PYADAMAS_TIMING"))
            std::fprintf(stderr, "[timing] codegen alloc=%.1f emit=%.1f ms\n",
                         alloc_ns_ / 1e6, emit_ns_ / 1e6);
        return std::move(out_);
    }

std::string generate_asm(const IRModule& m, bool disable_unknown_index) {
    AsmGen g;
    g.set_disable_unknown_index(disable_unknown_index);
    return g.gen(m);
}

}  // namespace pyadamas
