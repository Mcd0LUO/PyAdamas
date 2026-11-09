// codegen.cpp —— IR → x86-64 汇编：数据段、函数遍历与总入口
#include "codegen_impl.h"
#include "codegen.h"

namespace py23 {

std::string AsmGen::gen(const IRModule& m) {
        m_ = &m;
        opt_ = m.opt_level;
        out_.clear();

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
        // vtable 前固定 5 个字的元数据：MRO 元数据指针表、类编号、
        // vtable 自身、本类声明的槽表、模块限定类名。
        if (!m.classes.empty()) {
            out_ += "\t.section .data.rel.ro,\"aw\",@progbits\n";
            for (size_t i = 0; i < m.classes.size(); ++i) {
                out_ += "\t.align 8\n";
                out_ += std::format(".Lvtmeta{}:\n", i);
                out_ += std::format("\t.quad .Lvtmro{}\n", i);
                out_ += std::format("\t.quad {}\n", i);
                out_ += std::format("\t.quad .Lvt{}\n", i);
                out_ += std::format("\t.quad .Lvtown{}\n", i);
                out_ += std::format("\t.quad .Lcname{}\n", i);
                out_ += std::format(".Lvt{}:\n", i);
                if (m.classes[i].vtable.empty()) out_ += "\t.quad 0\n";
                for (int f : m.classes[i].vtable)
                    out_ += f < 0 ? "\t.quad 0\n" : std::format("\t.quad {}\n", func_label(f));
                out_ += std::format(".Lvtmro{}:\n", i);
                out_ += std::format("\t.quad {}\n", m.classes[i].mro.size());
                for (int a : m.classes[i].mro)
                    out_ += std::format("\t.quad .Lvtmeta{}\n", a);
                out_ += std::format(".Lvtown{}:\n", i);
                out_ += std::format("\t.quad {}\n", m.classes[i].own_slots.size());
                for (int s : m.classes[i].own_slots)
                    out_ += std::format("\t.quad {}\n", s);
                out_ += std::format(".Lcname{}:\n", i);
                out_ += std::format("\t.asciz \"{}\"\n", asm_escape(m.classes[i].qualname));
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

        out_ += "\t.text\n";

        // 每个有堆字段的类生成一个析构器，对象 rc 归零时由运行时回调。
        // 只 dec 堆字段，字段偏移与对象布局一致（父类字段在前）。
        for (size_t i = 0; i < m.classes.size(); ++i) {
            const IRClass& c = m.classes[i];
            bool has_heap = std::ranges::any_of(c.field_heap, [](uint8_t v) { return v != 0; });
            if (!has_heap) continue;
            out_ += "\n# ---- dtor class " + c.name + " ----\n";
            out_ += "\t.align 8\n";
            out_ += std::format(".Ldtor{}:\n", i);
            out_ += "\tpushq %rbx\n";
            out_ += "\tmovq %rdi, %rbx\n";
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                out_ += std::format("\tmovq {}(%rbx), %rdi\n", 8 * (f + 1));
                out_ += "\tcall py23_rc_dec\n";
            }
            out_ += "\tpopq %rbx\n";
            out_ += "\tret\n";

            // GC 清扫用：只把指向可达对象的字段引用减掉，不释放任何东西。
            out_ += "\n# ---- gc release class " + c.name + " ----\n";
            out_ += "\t.align 8\n";
            out_ += std::format(".Lgcrel{}:\n", i);
            out_ += "\tpushq %rbx\n";
            out_ += "\tmovq %rdi, %rbx\n";
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                out_ += std::format("\tmovq {}(%rbx), %rdi\n", 8 * (f + 1));
                out_ += "\tcall py23_gc_unref_child\n";
            }
            out_ += "\tpopq %rbx\n";
            out_ += "\tret\n";
        }

        // 生成器帧的 GC release：只把指向可达对象的槽引用减掉。正常结束
        // 时 status==1、槽位已经释放过，直接跳过；被中途抛弃时逐槽处理。
        // 析构器由运行时统一处理（它会调用生成代码的 close 路径跑 finally/with）。
        for (size_t i = 0; i < m.generators.size(); ++i) {
            const IRGenerator& g = m.generators[i];
            out_ += std::format("\n# ---- gcrel gen g{} ----\n", i);
            out_ += "\t.align 8\n";
            out_ += std::format(".Lgengcrel{}:\n", i);
            out_ += "\tpushq %rbx\n";
            out_ += "\tmovq %rdi, %rbx\n";
            out_ += "\tcmpq $1, 8(%rbx)\n";
            out_ += std::format("\tje .Lgengcreldone{}\n", i);
            for (size_t s = 0; s < g.slot_heap.size(); ++s) {
                if (!g.slot_heap[s]) continue;
                out_ += std::format("\tmovq {}(%rbx), %rdi\n", 8 * (10 + s));
                out_ += "\tcall py23_gc_unref_child\n";
            }
            out_ += std::format(".Lgengcreldone{}:\n", i);
            out_ += "\tpopq %rbx\n";
            out_ += "\tret\n";
        }

        for (size_t i = 0; i < m.funcs.size(); ++i) emit_function(*m.funcs[i], static_cast<int>(i));

        if (!m.is_entry && !m.module_symbol.empty() && m.entry >= 0) {
            out_ += "\t.globl " + module_init_symbol(m.module_symbol) + "\n";
            out_ += "\t.type " + module_init_symbol(m.module_symbol) + ", @function\n";
            out_ += module_init_symbol(m.module_symbol) + ":\n";
            out_ += "\tpushq %rbp\n";
            out_ += "\tmovq %rsp, %rbp\n";
            out_ += "\tleaq .Lstrtab(%rip), %rdi\n";
            out_ += std::format("\tmovq ${}, %rsi\n", m.strings.size());
            out_ += "\tcall py23_register_literals\n";
            out_ += "\tpopq %rbp\n";
            out_ += "\tjmp " + func_label(m.entry) + "\n";
        }

        // 真正的入口：调用降级出来的 __py23_main，返回它的返回值
        if (m.is_entry) {
            out_ += "\t.globl main\n";
            out_ += "\t.type main, @function\n";
            out_ += "main:\n";
            out_ += "\tpushq %rbp\n";
            out_ += "\tmovq %rsp, %rbp\n";
            out_ += "\tsubq $16, %rsp\n";
            out_ += "\tmovq %rdi, (%rsp)\n";
            out_ += "\tmovq %rsi, 8(%rsp)\n";
            // GC 根表 + 栈顶。栈顶取 main 的 rbp，收集器向下扫描。
            out_ += std::format("\tleaq {}(%rip), %rdi\n", roots_label());
            out_ += std::format("\tmovq ${}, %rsi\n", local_global_count());
            out_ += "\tmovq %rbp, %rdx\n";
            out_ += "\tcall py23_gc_init\n";
            out_ += "\tleaq .Lstrtab(%rip), %rdi\n";
            out_ += std::format("\tmovq ${}, %rsi\n", m.strings.size());
            out_ += "\tcall py23_register_literals\n";
            out_ += "\tmovslq (%rsp), %rdi\n";
            out_ += "\tmovq 8(%rsp), %rsi\n";
            out_ += "\tcall py23_sys_set_args\n";
            out_ += "\taddq $16, %rsp\n";
            if (m.entry >= 0) out_ += std::format("\tcall {}\n", func_label(m.entry));
            out_ += "\tpopq %rbp\n";
            out_ += "\tret\n";
        }
        out_ += "\t.section .note.GNU-stack,\"\",@progbits\n";

        return std::move(out_);
    }

std::string generate_asm(const IRModule& m) {
    AsmGen g;
    return g.gen(m);
}

}  // namespace py23
