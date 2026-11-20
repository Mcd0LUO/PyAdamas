// codegen_cffi.cpp —— C 互操作边界编组（cimport/extern）
// 
// 从 codegen_call.cpp 拆出（文件规模硬限 1200 行，P1 收尾新增窄宽度
// 编组后超限）。纯代码移动：gen_call_extern / gen_load_extern /
// gen_store_extern / gen_ptr_load / gen_ptr_store，函数体逐字不变。
#include "codegen_impl.h"

namespace pyadamas {

void AsmGen::gen_call_extern(const Inst& in) {
        const IRExtern& ex = m_->externs[static_cast<size_t>(in.idx)];

        // 同一符号的不同调用点可能有不同编组（ptr 接地址或 list/bytes），
        // 优先用调用点记录的 cargs。
        auto kind_at = [&](size_t i) -> CKind {
            if (i < in.cargs.size()) return static_cast<CKind>(in.cargs[i]);
            if (i < ex.params.size()) return ex.params[i];
            return CKind::Int;  // 可变参数多出来的实参
        };
        // 按值结构体的 SysV 分类；非结构体参数返回 nullptr。
        auto abi_at = [&](size_t i) -> const IRStructABI* {
            if (kind_at(i) != CKind::Struct || i >= ex.param_abi.size()) return nullptr;
            const IRStructABI& a = ex.param_abi[i];
            return a.size > 0 ? &a : nullptr;
        };

        // 一个实参的落点：标量占一个寄存器或 8 字节栈槽；结构体按 chunk
        // 分散到整数/xmm 寄存器，放不下时整块走栈。
        struct Place {
            bool is_struct = false;
            bool fp = false;
            int reg = -1;
            int stack_off = -1;
            int64_t size = 0;
            std::vector<uint8_t> cfp;
            std::vector<int> creg;
        };

        // 返回 >16 字节结构体时 %rdi 是隐藏的结果指针，整数参数整体后移。
        const bool hidden = ex.ret == CKind::Struct && ex.ret_abi.size > 16;
        // 可变参数调用可能比声明的 params 多，落点表按实参个数分配。
        size_t nargs = in.args.size() > ex.params.size() ? in.args.size() : ex.params.size();
        std::vector<Place> ps(nargs);
        int gi = hidden ? 1 : 0, fi = 0, sp = 0;
        for (size_t i = 0; i < nargs; ++i) {
            Place& p = ps[i];
            const IRStructABI* abi = i < ex.params.size() ? abi_at(i) : nullptr;
            if (!abi) {
                CKind kk = kind_at(i);
                bool fp = kk == CKind::Float || kk == CKind::Float32;
                p.fp = fp;
                if (fp && fi < kMaxFpArgs()) {
                    p.reg = fi++;
                } else if (!fp && gi < kMaxIntArgs()) {
                    p.reg = gi++;
                } else {
                    p.stack_off = out_base() + sp++ * 8;
                }
                continue;
            }
            p.is_struct = true;
            p.size = abi->size;
            p.cfp = abi->chunk_fp;
            int ni = 0, nf = 0;
            for (uint8_t c : p.cfp) {
                if (c) ++nf;
                else ++ni;
            }
            if (p.size > 16 || gi + ni > kMaxIntArgs() || fi + nf > kMaxFpArgs()) {
                p.stack_off = out_base() + sp * 8;
                sp += static_cast<int>(p.cfp.size());
                continue;
            }
            p.creg.assign(p.cfp.size(), -1);
            for (size_t c = 0; c < p.cfp.size(); ++c) p.creg[c] = p.cfp[c] ? fi++ : gi++;
        }

        int stack_bytes = sp * 8;
        int int_tmp0 = out_base() + stack_bytes + 8, int_tmp1 = out_base() + stack_bytes + 16;
        int fp_tmp0 = out_base() + stack_bytes + 24, fp_tmp1 = out_base() + stack_bytes + 32;
        int sret_tmp = out_base() + stack_bytes + 40;
        int fn_tmp = out_base() + stack_bytes + 48;

        // 取实参的机器值。str/可选 str 要先从 PyAdamasStr 里取出 data 字段
        // （偏移 24：pub/len/hash 之后的第四个机器字）；Buf* 从 list/bytes 的
        // data 取指针；普通指针按整数直接传。
        auto arg_into = [&](size_t i, const std::string& reg) {
            CKind k = kind_at(i);
            const std::string src = src_op(in.args[i]);
            if (k == CKind::Str || k == CKind::OptStr) {
                emit2("movq", src, "%r10");
                // 可空/可能为 NULL 的字符串：NULL 原样传，不能解引用。
                // 非可空 str 在类型检查阶段已拒绝 Optional[str]，这里只是
                // 防御运行期出现的空值。
                line("testq %r10, %r10");
                line("je 1f");
                line("movq 24(%r10), %r10");
                line("1:");
                emit2("movq", "%r10", reg);
            } else if (k == CKind::BufF64 || k == CKind::BufI64) {
                // list 的 data 在 +16（PyAdamasList）。
                emit2("movq", src, "%r10");
                line("testq %r10, %r10");
                line("je 1f");
                line("movq 16(%r10), %r10");
                line("1:");
                emit2("movq", "%r10", reg);
            } else if (k == CKind::BufBytes) {
                // bytes 的 data 在 +24（PyAdamasStr 的第四个机器字）。
                emit2("movq", src, "%r10");
                line("testq %r10, %r10");
                line("je 1f");
                line("movq 24(%r10), %r10");
                line("1:");
                emit2("movq", "%r10", reg);
            } else if (k == CKind::Callback) {
                // 先把闭包登记到签名槽，再把跳板地址作为 C 函数指针传入。
                int sig = (i < ex.param_cb.size()) ? ex.param_cb[i] : -1;
                emit2("movq", src, "%r10");
                if (sig >= 0) {
                    line(std::format("movq %r10, .Lcbslot{}(%rip)", sig));
                    line(std::format("leaq .Lcbtramp{}(%rip), %r10", sig));
                }
                emit2("movq", "%r10", reg);
            } else if (k == CKind::Float32) {
                // 栈上的 C float 实参：double 收窄成单精度，位模式进 8 字节槽。
                line(std::format("cvtsd2ss {}, %xmm0", src));
                emit2("movq", "%xmm0", reg);
            } else {
                emit2("movq", src, reg);
            }
        };

        // 结构体 chunk 搬运：不足 8 字节的尾部按实际宽度读写，避免碰
        // payload 之外的内存；读出的高字节零扩展即可（C 不要求 padding 值）。
        auto load_chunk = [&](const std::string& base, int64_t off, int64_t bytes) {
            if (bytes >= 8) line(std::format("movq {}({}), %r10", off, base));
            else if (bytes >= 4) line(std::format("movl {}({}), %r10d", off, base));
            else if (bytes >= 2) line(std::format("movzwl {}({}), %r10d", off, base));
            else line(std::format("movzbl {}({}), %r10d", off, base));
        };
        auto store_chunk = [&](const std::string& base, int64_t off, int64_t bytes) {
            if (bytes >= 8) line(std::format("movq %r10, {}({})", off, base));
            else if (bytes >= 4) line(std::format("movl %r10d, {}({})", off, base));
            else if (bytes >= 2) line(std::format("movw %r10w, {}({})", off, base));
            else line(std::format("movb %r10b, {}({})", off, base));
        };


        // 隐藏返回指针要在装配实参之前分配好对象（调用 pa_struct_new 会
        // 破坏参数寄存器），指针暂存在栈上，最后再装进 %rdi。
        if (hidden) {
            emit_imm_op("movq", ex.ret_abi.size, arg_reg_i(0));
            spill_fp_slots();
            line("call pa_struct_new");
            reload_fp_slots();
            emit2("movq", "%rax", std::format("{}(%rsp)", sret_tmp));
        }

        // 免写直调：先把 C 函数地址解析到 fn_tmp，再装配实参（解析调用
        // 会破坏参数寄存器）。lazy==1 走进程默认符号表并缓存到静态槽；
        // lazy==2 每次按运行期句柄 dlsym（句柄在 in.a）。
        if (ex.lazy) {
            spill_fp_slots();
            if (ex.lazy == 1) {
                line(std::format("movq .Lcsym{}(%rip), %r11", ex.sym_name));
                line("testq %r11, %r11");
                std::string hit = fresh_label("csymhit");
                emit_jmp("jne", hit);
                line(std::format("leaq .Lcsymstr{}(%rip), {}", ex.sym_name, arg_reg_i(0)));
                line("call pa_c_sym_default");
                line("movq %rax, %r11");
                line(std::format("movq %r11, .Lcsym{}(%rip)", ex.sym_name));
                label(hit);
            } else {
                emit2("movq", src_op(in.a), arg_reg_i(0));
                line(std::format("leaq .Lcsymstr{}(%rip), {}", ex.sym_name, arg_reg_i(1)));
                line("call pa_c_sym_handle");
                line("movq %rax, %r11");
            }
            line(std::format("movq %r11, {}(%rsp)", fn_tmp));
            reload_fp_slots();
        }

        // slot_off >= 0 表示整块走栈，否则按 chunk 分散到寄存器。
        auto struct_arg = [&](size_t i, const Place& p, int slot_off) {
            emit2("movq", src_op(in.args[i]), "%r11");
            for (size_t c = 0; c < p.cfp.size(); ++c) {
                int64_t off = static_cast<int64_t>(c) * 8;
                int64_t bytes = p.size - off < 8 ? p.size - off : 8;
                if (slot_off >= 0) {
                    if (p.cfp[c]) {
                        line(std::format("movsd {}(%r11), %xmm0", off));
                        line(std::format("movsd %xmm0, {}(%rsp)", slot_off + off));
                    } else {
                        load_chunk("%r11", off, bytes);
                        line(std::format("movq %r10, {}(%rsp)", slot_off + off));
                    }
                } else if (p.cfp[c]) {
                    line(std::format("movsd {}(%r11), %xmm{}", off, p.creg[c]));
                } else {
                    load_chunk("%r11", off, bytes);
                    emit2("movq", "%r10", arg_reg_i(p.creg[c]));
                }
            }
        };

        for (size_t i = 0; i < in.args.size(); ++i) {
            if (ps[i].stack_off < 0) continue;
            if (ps[i].is_struct) {
                struct_arg(i, ps[i], ps[i].stack_off);
            } else {
                arg_into(i, "%r10");
                line(std::format("movq %r10, {}(%rsp)", ps[i].stack_off));
            }
        }
        for (size_t i = 0; i < in.args.size(); ++i) {
            const auto& p = ps[i];
            if (p.stack_off >= 0) continue;
            if (p.is_struct) {
                struct_arg(i, p, -1);
            } else if (p.fp) {
                if (kind_at(i) == CKind::Float32)
                    line(std::format("cvtsd2ss {}, %xmm{}", src_op(in.args[i]), p.reg));
                else
                    line(std::format("movsd {}, %xmm{}", src_op(in.args[i]), p.reg));
            } else {
                arg_into(i, arg_reg_i(p.reg));
            }
        }
        if (hidden) emit2("movq", std::format("{}(%rsp)", sret_tmp), arg_reg_i(0));
        // 可变参数函数要求 %al = 使用的向量寄存器数；非可变参数沿用
        // 原有的清空做法。
        // %al 是「用到的向量寄存器数」，ABI 上限 8：溢出的浮点实参走栈，
        // 不能把 fi 原样塞进去（>8 会让 va_arg 的寄存器保存区读越界）。
        if (ex.varargs) line(std::format("movl ${}, %eax", fi > 8 ? 8 : fi));
        else if (opt_ < 2) line("xorl %eax, %eax");
        spill_fp_slots();
        if (ex.lazy) {
            line(std::format("movq {}(%rsp), %r11", fn_tmp));
            line("call *%r11");
        } else {
            line("call " + ex.symbol);
        }
        reload_fp_slots();

        if (!dst_unused(in)) {
            std::string dst = dst_op(in.dst);
            if (ex.ret == CKind::Struct && ex.ret_abi.size > 0) {
                const IRStructABI& ra = ex.ret_abi;
                if (ra.size > 16) {
                    emit2("movq", std::format("{}(%rsp)", sret_tmp), "%rax");
                    move_to(dst, "%rax");
                } else {
                    // SysV：INTEGER chunk 依次在 rax/rdx，SSE chunk 在
                    // xmm0/xmm1。先暂存返回值，再分配对象并写回 payload。
                    int ni = 0, nf = 0;
                    std::vector<int> ic(ra.chunk_fp.size(), -1), fc(ra.chunk_fp.size(), -1);
                    for (size_t c = 0; c < ra.chunk_fp.size(); ++c) {
                        if (ra.chunk_fp[c]) fc[c] = nf++;
                        else ic[c] = ni++;
                    }
                    if (ni > 0) emit2("movq", "%rax", std::format("{}(%rsp)", int_tmp0));
                    if (ni > 1) emit2("movq", "%rdx", std::format("{}(%rsp)", int_tmp1));
                    if (nf > 0) line(std::format("movsd %xmm0, {}(%rsp)", fp_tmp0));
                    if (nf > 1) line(std::format("movsd %xmm1, {}(%rsp)", fp_tmp1));
                    emit_imm_op("movq", ra.size, arg_reg_i(0));
                    spill_fp_slots();
                    line("call pa_struct_new");
                    reload_fp_slots();
                    emit2("movq", "%rax", "%r11");
                    for (size_t c = 0; c < ra.chunk_fp.size(); ++c) {
                        int64_t off = static_cast<int64_t>(c) * 8;
                        int64_t bytes = ra.size - off < 8 ? ra.size - off : 8;
                        if (ra.chunk_fp[c]) {
                            int t = fc[c] == 0 ? fp_tmp0 : fp_tmp1;
                            line(std::format("movsd {}(%rsp), %xmm0", t));
                            line(std::format("movsd %xmm0, {}(%r11)", off));
                        } else {
                            int t = ic[c] == 0 ? int_tmp0 : int_tmp1;
                            line(std::format("movq {}(%rsp), %r10", t));
                            store_chunk("%r11", off, bytes);
                        }
                    }
                    move_to(dst, "%r11");
                }
            } else {
                switch (ex.ret) {
                    case CKind::None:
                        break;
                    case CKind::Float:
                        emit2("movsd", "%xmm0", dst);
                        break;
                    case CKind::Float32:
                        line("cvtss2sd %xmm0, %xmm0");
                        emit2("movsd", "%xmm0", dst);
                        break;
                    case CKind::Int32:
                        line("movslq %eax, %rax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::UInt32:
                        line("movl %eax, %eax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::Int8:
                        line("movsbq %al, %rax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::UInt8:
                        line("movzbq %al, %rax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::Int16:
                        line("movswq %ax, %rax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::UInt16:
                        line("movzwq %ax, %rax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::UInt64:
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::Bool:
                        line("testq %rax, %rax");
                        line("setne %al");
                        line("movzbl %al, %eax");
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::Str:
                    case CKind::OptStr: {
                        // C 返回的 char* 不能直接交给 PyAdamas：需要复制成
                        // PyAdamasStr。NULL 对 str 视为空串，对 Optional[str]
                        // 保持 None。
                        spill_fp_slots();
                        emit2("movq", "%rax", arg_reg_i(0));
                        line(ex.ret == CKind::OptStr ? "call pa_str_from_cstr_opt"
                                                     : "call pa_str_from_cstr");
                        reload_fp_slots();
                        emit2("movq", "%rax", dst);
                        break;
                    }
                    case CKind::Struct:
                    case CKind::Callback:
                        // 理论上不会作为返回值出现；按指针透传。
                        emit2("movq", "%rax", dst);
                        break;
                    case CKind::Int:
                    case CKind::Ptr:
                    case CKind::BufF64:
                    case CKind::BufI64:
                    case CKind::BufBytes:
                        emit2("movq", "%rax", dst);
                        break;
                }
            }
        }
    }

// C 全局变量：按声明宽度从符号地址加载。可执行文件里的数据符号用
// RIP 相对直接寻址（PIE 下由 copy relocation 处理）。
void AsmGen::gen_load_extern(const Inst& in) {
        const IRExternVar& v = m_->extern_vars[static_cast<size_t>(in.idx)];
        const std::string dst = dst_op(in.dst);
        const std::string sym = v.symbol + "(%rip)";
        switch (v.kind) {
            case CKind::Float:
                line("movsd " + sym + ", %xmm0");
                emit2("movsd", "%xmm0", dst);
                break;
            case CKind::Float32:
                line("movss " + sym + ", %xmm0");
                line("cvtss2sd %xmm0, %xmm0");
                emit2("movsd", "%xmm0", dst);
                break;
            case CKind::Int32:
                line("movslq " + sym + ", %rax");
                move_to(dst, "%rax");
                break;
            case CKind::UInt32:
                line("movl " + sym + ", %eax");
                move_to(dst, "%rax");
                break;
            case CKind::Int8:
                line("movsbq " + sym + ", %rax");
                move_to(dst, "%rax");
                break;
            case CKind::UInt8:
                line("movzbq " + sym + ", %rax");
                move_to(dst, "%rax");
                break;
            case CKind::Int16:
                line("movswq " + sym + ", %rax");
                move_to(dst, "%rax");
                break;
            case CKind::UInt16:
                line("movzwq " + sym + ", %rax");
                move_to(dst, "%rax");
                break;
            case CKind::Bool:
                line("movzbl " + sym + ", %eax");
                move_to(dst, "%rax");
                break;
            case CKind::Str:
            case CKind::OptStr:
                // 全局 char* 同样要复制成 PyAdamasStr。
                line("movq " + sym + ", %rdi");
                spill_fp_slots();
                line(v.kind == CKind::OptStr ? "call pa_str_from_cstr_opt"
                                             : "call pa_str_from_cstr");
                reload_fp_slots();
                move_to(dst, "%rax");
                break;
            default:
                line("movq " + sym + ", %rax");
                move_to(dst, "%rax");
                break;
        }
    }

void AsmGen::gen_store_extern(const Inst& in) {
        const IRExternVar& v = m_->extern_vars[static_cast<size_t>(in.idx)];
        const std::string sym = v.symbol + "(%rip)";
        switch (v.kind) {
            case CKind::Float:
                emit2("movsd", src_op(in.a), "%xmm0");
                line("movsd %xmm0, " + sym);
                break;
            case CKind::Float32:
                emit2("movsd", src_op(in.a), "%xmm0");
                line("cvtsd2ss %xmm0, %xmm0");
                line("movss %xmm0, " + sym);
                break;
            case CKind::Int32:
            case CKind::UInt32:
                emit2("movq", src_op(in.a), "%rax");
                line("movl %eax, " + sym);
                break;
            case CKind::Int8:
            case CKind::UInt8:
                emit2("movq", src_op(in.a), "%rax");
                line("movb %al, " + sym);
                break;
            case CKind::Int16:
            case CKind::UInt16:
                emit2("movq", src_op(in.a), "%rax");
                line("movw %ax, " + sym);
                break;
            case CKind::Bool:
                emit2("movq", src_op(in.a), "%rax");
                line("movb %al, " + sym);
                break;
            default:
                emit2("movq", src_op(in.a), "%rax");
                line("movq %rax, " + sym);
                break;
        }
    }

// C 指针解引用：地址 = a + b * 元素宽度。宽度是 1/2/4/8 时直接作为
// leaq 的比例因子。
void AsmGen::gen_ptr_load(const Inst& in) {
        emit2("movq", src_op(in.a), "%rax");
        emit2("movq", src_op(in.b), "%r11");
        line(std::format("leaq (%rax,%r11,{}), %rax", in.imm));
        const std::string dst = dst_op(in.dst);
        if (ir_is_float(in.ty)) {
            if (in.fwidth == 4) {
                // ptr[float32]：单精度装载后提升成 double。
                line("movss (%rax), %xmm0");
                line("cvtss2sd %xmm0, %xmm0");
            } else {
                line("movsd (%rax), %xmm0");
            }
            emit2("movsd", "%xmm0", dst);
            return;
        }
        if (in.fwidth == 4) {
            if (in.funsigned) line("movl (%rax), %eax");
            else line("movslq (%rax), %rax");
        } else if (in.fwidth == 1) {
            //  signed char 解引用要符号扩展；Bool/无符号仍零扩展（旧行为）。
            if (in.ty && in.ty->kind == TyKind::I8) line("movsbq (%rax), %rax");
            else line("movzbl (%rax), %eax");
        } else if (in.fwidth == 2) {
            if (in.ty && in.ty->kind == TyKind::I16) line("movswq (%rax), %rax");
            else line("movzwl (%rax), %eax");
        } else {
            line("movq (%rax), %rax");
        }
        move_to(dst, "%rax");
    }

void AsmGen::gen_ptr_store(const Inst& in) {
        emit2("movq", src_op(in.a), "%rax");
        emit2("movq", src_op(in.b), "%r11");
        line(std::format("leaq (%rax,%r11,{}), %rax", in.imm));
        if (ir_is_float(in.ty)) {
            emit2("movsd", src_op(in.c), "%xmm0");
            if (in.fwidth == 4) {
                line("cvtsd2ss %xmm0, %xmm0");
                line("movss %xmm0, (%rax)");
            } else {
                line("movsd %xmm0, (%rax)");
            }
            return;
        }
        emit2("movq", src_op(in.c), "%r10");
        if (in.fwidth == 4) line("movl %r10d, (%rax)");
        else if (in.fwidth == 1) line("movb %r10b, (%rax)");
        else if (in.fwidth == 2) line("movw %r10w, (%rax)");
        else line("movq %r10, (%rax)");
    }

}  // namespace pyadamas
