// codegen_emit.h —— AsmGen 的操作数工厂与迷你发射器
//
// 从 codegen_impl.h 拆出（文件规模闸门 1200 行）。本文件是 class AsmGen
// 的类体片段：由 codegen_impl.h 在类内 #include，定义的都是 AsmGen 成员，
// 不单独编译。拆出的只是「操作数怎么建、指令怎么发」这一层机器。
//
// 发射路径分两条：
//   * 结构化直建（op_* 工厂 + emit2/emit_imm_op/emit_jmp 的成功路径）：
//     操作数直接落成 MOperand（寄存器编号/立即数/结构化内存），不经过
//     「拼字符串 → parse_operand 回解析」的往返；
//   * 文本 fallback：助记符没进 MOp 枚举、或操作数文法不认识时，原样
//     拼文本走 push_line（认不出退化 Raw），保证字节不变。
// 注意：本文件无 #pragma once / namespace 包裹——它是类体片段，
// 预处理直接展开进 class AsmGen { ... } 内部。

    // 操作数工厂。寄存器按编号存（op_reg(kRax)），名字查静态表；
    // 按名字构造时查表得编号，表外名字渲染走 text 兜底。
    // 所有工厂同时填 canonical text（与 render 逐字节一致），供字符串接口用。
    static MOperand op_reg(MReg r) {
        MOperand o;
        o.kind = MOperandKind::Reg;
        o.reg = r;
        o.text = mreg_name(r);
        return o;
    }
    static MOperand op_reg(std::string name) {
        MOperand o;
        o.kind = MOperandKind::Reg;
        o.reg = mreg_from_name(name);
        o.text = std::move(name);
        return o;
    }
    static MOperand op_imm(int64_t v) {
        MOperand o;
        o.kind = MOperandKind::Imm;
        o.imm = v;
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, v);
        o.text = "$";
        o.text.append(buf, r.ptr);
        return o;
    }
    static MOperand op_imm_hex(uint64_t bits) {
        MOperand o;
        o.kind = MOperandKind::Imm;
        o.imm = static_cast<int64_t>(bits);
        o.imm_hex = true;
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, bits, 16);
        o.text = "$0x";
        o.text.append(buf, r.ptr);
        return o;
    }
    static MOperand op_label(std::string name) {
        MOperand o;
        o.kind = MOperandKind::Label;
        o.text = std::move(name);
        return o;
    }
    static MOperand op_sym(std::string name) {
        MOperand o;
        o.kind = MOperandKind::Sym;
        o.text = std::move(name);
        return o;
    }
    // "(%base)"
    static MOperand op_mem(std::string base) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.reg = mreg_from_name(base);
        o.text = "(" + base + ")";
        return o;
    }
    // "disp(%base)"
    static MOperand op_mem(std::string base, int64_t disp) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.reg = mreg_from_name(base);
        o.imm = disp;
        o.has_disp = true;
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, disp);
        o.text.assign(buf, r.ptr);
        o.text += '(';
        o.text += base;
        o.text += ')';
        return o;
    }
    // "sym(%base)"
    static MOperand op_mem_sym(std::string sym, std::string base) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.reg = mreg_from_name(base);
        o.has_sym = true;
        o.text = std::move(sym);
        o.text += '(';
        o.text += base;
        o.text += ')';
        return o;
    }

    // 热路径的迷你发射器：直接构造结构化指令，绕开 std::format。
    // 助记符没进 MOp 枚举、或操作数文法不认识时退回文本路径（Raw，字节不变）。
    void emit2(const char* mn, const std::string& a, const std::string& b) {
        MOp op = mop_from_name(mn);
        MOperand oa;
        MOperand ob;
        if (op != MOp::kRaw && parse_operand(a, false, &oa) && parse_operand(b, false, &ob)) {
            MInstr ins;
            ins.op = op;
            ins.ops.push_back(std::move(oa));
            ins.ops.push_back(std::move(ob));
            out_.push_instr(std::move(ins));
            return;
        }
        std::string text = mn;
        text += ' ';
        text += a;
        text += ", ";
        text += b;
        out_.push_line(text);
    }

    void emit_imm_op(const char* mn, int64_t v, const std::string& b) {
        MOp op = mop_from_name(mn);
        MOperand ob;
        if (op != MOp::kRaw && parse_operand(b, false, &ob)) {
            MInstr ins;
            ins.op = op;
            ins.ops.push_back(op_imm(v));
            ins.ops.push_back(std::move(ob));
            out_.push_instr(std::move(ins));
            return;
        }
        char buf[32];
        auto r = std::to_chars(buf, buf + sizeof buf, v);
        std::string text = mn;
        text += " $";
        text.append(buf, r.ptr);
        text += ", ";
        text += b;
        out_.push_line(text);
    }

    void emit_jmp(const char* mn, const std::string& target) {
        MOp op = mop_from_name(mn);
        MOperand ot;
        if (op != MOp::kRaw && parse_operand(target, mop_is_branch(op), &ot)) {
            MInstr ins;
            ins.op = op;
            ins.ops.push_back(std::move(ot));
            out_.push_instr(std::move(ins));
            return;
        }
        std::string text = mn;
        text += ' ';
        text += target;
        out_.push_line(text);
    }

    void emit_imm_mem(const std::string& mem, int64_t v) { emit_imm_op("movq", v, mem); }
