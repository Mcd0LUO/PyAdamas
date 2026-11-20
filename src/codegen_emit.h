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
    // "(%base)" / "disp(%base)" —— 寄存器编号版（零名字查找）。
    static MOperand op_mem(MReg base) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.reg = base;
        o.text = "(";
        o.text += mreg_name(base);
        o.text += ')';
        return o;
    }
    static MOperand op_mem(MReg base, int64_t disp) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.reg = base;
        o.imm = disp;
        o.has_disp = true;
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, disp);
        o.text.assign(buf, r.ptr);
        o.text += '(';
        o.text += mreg_name(base);
        o.text += ')';
        return o;
    }
    // "(%base,%index)" —— 无显式 scale（与历史文本 "(%r10,%rax)" 同字节）。
    static MOperand op_mem(MReg base, MReg index) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.reg = base;
        o.index = index;
        o.text = "(";
        o.text += mreg_name(base);
        o.text += ',';
        o.text += mreg_name(index);
        o.text += ')';
        return o;
    }
    // "(%base,%index,scale)" / "disp(%base,%index,scale)"
    static MOperand op_mem(MReg base, MReg index, int scale) {
        MOperand o;
        o.kind = MOperandKind::Mem;
        o.reg = base;
        o.index = index;
        o.scale = static_cast<uint8_t>(scale);
        o.has_scale = true;
        o.text = "(";
        o.text += mreg_name(base);
        o.text += ',';
        o.text += mreg_name(index);
        o.text += ',';
        char buf[8];
        auto r = std::to_chars(buf, buf + sizeof buf, scale);
        o.text.append(buf, r.ptr);
        o.text += ')';
        return o;
    }
    static MOperand op_mem(MReg base, MReg index, int scale, int64_t disp) {
        MOperand o = op_mem(base, index, scale);
        o.imm = disp;
        o.has_disp = true;
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, disp);
        std::string t(buf, r.ptr);
        t += o.text;
        o.text = std::move(t);
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

    // ── 结构化直发（块B2：免「拼字符串→parse_operand 回解析」往返）──
    // 操作数已是 MOperand 单元（缓存直取 / 工厂直建），拷贝进内联存储即发。
    // MReg 便捷重载走预建静态操作数表，零构造。const char* 实参经
    // MOperand 转换构造解析一次（ABI 寄存器名等），仍比整行文本路径便宜。
    void emit2(MOp op, const MOperand& a, const MOperand& b) {
        MInstr ins;
        ins.op = op;
        ins.ops.push_back(a);
        ins.ops.push_back(b);
        out_.push_instr(std::move(ins));
    }
    void emit2(MOp op, MReg a, const MOperand& b) { emit2(op, mreg_operand(a), b); }
    void emit2(MOp op, const MOperand& a, MReg b) { emit2(op, a, mreg_operand(b)); }
    void emit2(MOp op, MReg a, MReg b) { emit2(op, mreg_operand(a), mreg_operand(b)); }
    void emit1(MOp op, const MOperand& a) {
        MInstr ins;
        ins.op = op;
        ins.ops.push_back(a);
        out_.push_instr(std::move(ins));
    }
    void emit1(MOp op, MReg a) { emit1(op, mreg_operand(a)); }
    void emit0(MOp op) {
        MInstr ins;
        ins.op = op;
        out_.push_instr(std::move(ins));
    }
    void emit_imm(MOp op, int64_t v, const MOperand& b) {
        MInstr ins;
        ins.op = op;
        ins.ops.push_back(op_imm(v));
        ins.ops.push_back(b);
        out_.push_instr(std::move(ins));
    }
    void emit_imm(MOp op, int64_t v, MReg b) { emit_imm(op, v, mreg_operand(b)); }
    void emit_imm_hex(MOp op, uint64_t bits, const MOperand& b) {
        MInstr ins;
        ins.op = op;
        ins.ops.push_back(op_imm_hex(bits));
        ins.ops.push_back(b);
        out_.push_instr(std::move(ins));
    }
    void emit_imm_hex(MOp op, uint64_t bits, MReg b) { emit_imm_hex(op, bits, mreg_operand(b)); }
    // 跳转/调用目标直建：目标是标签/符号字符串时免 parse_operand 文法扫描。
    // 含特殊字符（间接 '*'、内存 '(' 等）的目标退回解析路径，字节不变。
    void emit_jmp(MOp op, std::string_view target) {
        // 裸符号/标签直建。'$' 允许出现在符号名里（.Lf6$pt 这类 mangled
        // 标签）：历史路径经 line() 落 Raw，渲染同样是 tab+助记符+目标。
        bool bare = !target.empty();
        for (char c : target) {
            if (c == ' ' || c == ',' || c == '(' || c == ')' || c == '*') {
                bare = false;
                break;
            }
        }
        if (bare) {
            MOperand ot;
            ot.kind = mop_is_branch(op) ? MOperandKind::Label : MOperandKind::Sym;
            ot.text = target;
            emit1(op, ot);
            return;
        }
        MOperand ot;
        if (parse_operand(target, mop_is_branch(op), &ot)) {
            emit1(op, ot);
            return;
        }
        // 兜底走 line()（带 tab），与历史 line(format("call ...")) 同字节。
        std::string text = mop_name(op);
        text += ' ';
        text += target;
        line(text);
    }
