// backend/core.h —— AsmGen 的骨架：位置状态 / 输出辅助 / 目标指示符 /
// 函数·基本块·指令·对象·调用（类体片段）
//
// **本文件是 class AsmGen 的类体片段**：由 backend/impl.h 在 class 体内
// #include，内容就是 AsmGen 的成员声明，不单独编译、没有 include guard。
// 不要在文件里写 namespace / 文件级 #include —— 那会出现在类体中间，编译失败。
//
// 从 backend/impl.h 按原有分组注释整段切出（纯文本移动，无逻辑改动）。
// 函数体已经搬进 backend/impl.cpp 的只剩声明；仍保留内联的是极热小工具：
// emit2 族 / op_* 操作数工厂 / mem·smem·slot·src·dst 查表 / line·label 入口。
//
// 分段说明：源码里有若干段没有分组注释（mem_mop…is_fp 的位置查找与搬运、
// block_stores_/cur_pos_ 的块内位置索引），它们夹在「零开销异常」与
// 「live range splitting」之间；按内容属于位置状态，收在本文件。

    // ── 位置状态（每次 emit_function 重建）──────────────────────
    //
    // ra_.slot_reg[s] 非空表示变量 s 常驻在这个寄存器里，栈槽作废。
    // 分配是函数级的、固定的：不做活跃性分析，一个变量要么整函数
    // 在寄存器里，要么整函数在栈上，规则简单到不会出隐蔽 bug。
    // 槽/跨块 vreg 的寄存器提升状态已拆到 RegAlloc（src/backend/reg_alloc.h）。
    RegAlloc ra_;
    int n_saved_regs_ = 0;  // 实际 push 了几个 callee-saved

    // 当前块内的位置索引，gen_block 开头重建。
    std::unordered_map<int, std::vector<int>> block_stores_;
    // 块内首次读位置 + RcMark 打标（见 common/block_bitset.h 里的 BlockUseIndex）
    BlockUseIndex block_use_;
    int cur_pos_ = 0;  // 正在生成的指令在块内的下标

    // ── 输出辅助（结构化 Machine 层）──────────────────────────────
    //
    // emit_.out 曾经是 std::string，逐段拼接 AT&T 文本；现在是 MachFunc：每条
    // 发射落成 MItem（Label/Directive/Instr/Comment/Raw）。指令结构化之后，
    // 后续的寄存器分配 / 指令选择 pass 可以直接改写 MInstr 的操作数，而
    // 不必只在发射前的字符串上做映射。
    //
    // 保留 line() 这个文本入口，是为了让历史发射点零改动地接进来：它把
    // 一行文本解析成结构化 item（认不出的文法退化 Raw，字节不变）。
    // 新写的发射点应优先用 instr()/directive()/label() 直接构造。

    // 兜底：原样文本进 Raw（与上一条 Raw 合并）。全部迁移完后应无调用点。
    void emit_raw(std::string_view s) { emit_.out.append_raw(s); }

    // 一行汇编：指令 / 指示符 / 标签 / 注释都走这里。
    // 含换行的多行文本直接进 Raw，保证字节不变。
    void line(std::string_view s) override {
        if (s.find('\n') != std::string_view::npos) {
            emit_raw("\t");
            emit_raw(s);
            emit_raw("\n");
            return;
        }
        // 旧实现是 "\t" + s + "\n"；这里复用缓冲区补上前导 tab，避免每行一次分配。
        emit_.line_buf.clear();
        emit_.line_buf += '\t';
        emit_.line_buf.append(s);
        emit_.out.push_line(emit_.line_buf);
    }

    // 静态字面量行（line("movq %rax, %rbx") 这类）：进程内只解析一次，
    // 之后直接复制结构化结果——RC 内联序列等固定指令模板每次发射都重新
    // parse_instr/parse_operand 是纯浪费。按内容做透明哈希键（字面量地址
    // 可能被链接器合并/变化，内容才是稳定标识）。编译器单线程，静态缓存
    // 无锁；键只来自字面量与 ABI 静态串，规模有界。
    void line(const char* s) {
        struct SvHash {
            using is_transparent = void;
            size_t operator()(std::string_view v) const {
                return std::hash<std::string_view>{}(v);
            }
        };
        struct Entry { MItem item; int32_t inst = -1; };
        struct Cache {
            std::unordered_map<std::string, Entry, SvHash, std::equal_to<>> map;
            std::vector<MInstr> insts;  // 缓存指令池（Entry.inst 下标）
        };
        static Cache cache;
        const std::string_view sv(s);
        auto it = cache.map.find(sv);
        if (it == cache.map.end()) {
            Entry e;
            if (sv.find('\n') != std::string_view::npos) {
                e.item = make_raw("\t" + std::string(sv) + "\n");
            } else {
                emit_.line_buf.clear();
                emit_.line_buf += '\t';
                emit_.line_buf.append(sv);
                MachFunc tmp;
                parse_line_into(emit_.line_buf, tmp);
                e.item = std::move(tmp.items[0]);
                if (e.item.kind == MItem::Instr) {
                    e.inst = static_cast<int32_t>(cache.insts.size());
                    cache.insts.push_back(std::move(tmp.insts[0]));
                    e.item.inst = -1;  // 下标按目标函数重定位，缓存里不存
                }
            }
            it = cache.map.emplace(std::string(sv), std::move(e)).first;
        }
        const Entry& e = it->second;
        if (e.inst >= 0)
            emit_.out.push_cached(e.item, cache.insts[static_cast<size_t>(e.inst)]);
        else
            emit_.out.push(e.item);
    }

    // 列 0 的标签（函数标签、数据段符号）。
    void label(const std::string& s) override { emit_.out.push_label(s, false); }
    // 直接塞一条已构造好的结构化指令。
    void emit_instr(MInstr&& ins) override { emit_.out.push_instr(std::move(ins)); }
    // 带前导 tab 的块内局部标签（"\t1:" 这种）。
    void label_block(const std::string& s) { emit_.out.push_label(s, true); }
    // "\t# s" / 指令注释 / 列 0 注释行（"# ..."）。实现见 backend/comment.cpp。
    void comment(std::string_view s); void comment_inst(const Inst& in); void comment0(std::string_view s);
    // "\t" + body + "\n"：.align/.quad/.section 这类汇编指示符。
    void directive(std::string_view body) { emit_.out.push_directive(body); }
    // 空行。
    void blank_line() { emit_.out.push(make_raw("\n")); }

    // 结构化指令发射：操作码 + 操作数。
    void instr(MOp op, std::initializer_list<MOperand> ops);
    // 操作数工厂 + 迷你发射器（emit2/emit_imm_op/emit_jmp）拆在 backend/emit.h，
    // 类体片段，就地展开（文件规模闸门）。
#include "backend/emit.h"
    // 向量化成员/声明拆在 backend/vect.h（类体片段，文件规模闸门）。
    // 0.480：特化 SIMD 模板（原 backend/simd*.cpp，PYADAMAS_SIMD）整段删除 ——
    // 对当前 IR 一次都没匹配上（12 基准 asm 零 SIMD 标记、指令数与关掉向量化
    // 逐位相同），见 docs/simd-vectorization-design.md 与 CHANGELOG 0.480。
#include "backend/vect_gates.h"
#include "backend/vect.h"
    // ── 目标相关的汇编指示符 ────────────────────────────────────
    // ELF/GNU as 的 .cfi_*、.type、.size、.note.GNU-stack 在 COFF 目标上
    // 要么非法要么被忽略，所以按 TargetABI::gnu_cfi 关掉。关掉后 Windows
    // 侧没有 .eh_frame 展开信息——那是 SEH 的地盘，需要单独实现。
    bool gnu_cfi() const { return target_abi().gnu_cfi; }

    // 出参区的基址。Win64 要求调用点在栈顶留 32 字节影子空间给被调方写
    // 寄存器实参，所以栈传实参和所有暂存都从 shadow_space 之上开始。
    // SysV 的 shadow_space 是 0，偏移与原来完全一致。
    int out_base() const { return target_abi().shadow_space; }

    // 搬运第 pos 个（0 基）实参到 ABI 规定的位置。is_fp 表示浮点类；
    // cls_idx 是该类内的序号（SysV 按类型分计数时用）。
    void emit_arg(int pos, bool is_fp, int cls_idx, const MOperand& v);

    void cfi(const std::string& s);
    // CFI 数字行直拼（to_chars）：序言/尾声每函数十几条，免 std::format。
    // 走 directive() 直发，不复用 line()（避免 emit_.line_buf 自引用）。
    void cfi_cfa_offset();
    void cfi_restore(const char* reg);
    void cfi_offset(const char* reg, int off);
    // relro 段：ELF 下是 .data.rel.ro，COFF 没有这个概念，退到 .rdata。
    void section_relro();
    void section_rodata();
    void emit_type_func(const std::string& n);
    void emit_size_func(const std::string& n);
    void emit_gnu_stack_note();

    // 数字后缀标签（.Lvt3 / .Ldtor2 这类）：to_chars 直拼。
    static std::string num_label(const char* prefix, int64_t n);

    // 标签工厂：to_chars 直拼，免 std::format（RC 序列每条 2-3 个标签，
    // 是 vformat 热点的大头）。
    std::string fresh_label(const char* tag) override {
        std::string s = ".L";
        s += tag;
        s += '_';
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, emit_.label_id++);
        s.append(buf, r.ptr);
        return s;
    }

    static std::string func_label(int i) {
        std::string s = ".Lf";
        char buf[24];
        auto r = std::to_chars(buf, buf + sizeof buf, i);
        s.append(buf, r.ptr);
        return s;
    }
    // 写槽（提升到 xmm 的槽 + 立即数需要特殊编码，见 backend/store.cpp）。
    void store_slot_checked(const Inst& in);
    std::string entry_ref(int i) const { return inline_opt_ ? func_label(i) + "e" : func_label(i); }
    // 统一调用约定的 shim 入口标签（见 ir/types.h 的 closure_needs_shim）。
    static std::string shim_label(int i);

    std::string block_label(int bi) const { return block_labels_[static_cast<size_t>(bi)]; }

    // 槽位一律相对 rsp：帧的布局是
    //   [出参区 frame_.out_bytes][槽位区 slot0…][帧记录][push 的 callee-saved][返回地址]
    // rsp 在函数体里从头到尾不动（出参区一次性预留，调用点不再 subq，
    // setjmp/longjmp 也只会回到同一个 rsp），所以偏移是常量。
    // 槽号小的（形参、最常用的局部）排在**低地址**，偏移能落在 disp8 里；
    // 帧记录这种每函数只碰一两次的放到最上面。
    int slot_off(int slot) const { return frame_.out_bytes + 8 * slot; }

    int vreg_off(VReg r) const;

    // 帧记录（PyAdamasFrame）在槽位区上面，内存里从低地址往高地址依次是
    // prev、n、临时区起点、临时区字数、临时区位图、本帧 CFA、槽地址×n，
    // 与两边共用的结构体布局一致。
    int rec_off(int i) const;
    MOperand rec_mem(int i) const { return op_mem(kRsp, rec_off(i)); }

    // 本次代码生成是否压了帧记录（弹栈只在这个函数里发）。
    bool frame_rec_ = false;
    // DWARF 行号：当前已发出的 .loc 行，切函数/切行时更新。
    int cur_loc_line_ = 0;
    // 生成指令前按需要发出 .loc；line<=0 或未变化时不动。
    void set_loc(int line);
    // 运行时 traceback 用的 PC → (函数, 行号) 表的一项。
    struct DbgEntry {
        std::string label;  // .LdbgN / .LdbgEndN
        int fi = -1;        // 函数下标；-1 表示函数结束哨兵
        int line = 0;
    };
    std::vector<DbgEntry> dbg_entries_;
    int dbg_seq_ = 0;
    bool dbg_on_ = false;
    std::string dbg_name_label(int fi) const;
    void emit_debug_sections(const IRModule& m);

    // vreg/槽的内存操作数：结构化单元优先（发射点直取免回解析），
    // 字符串接口委托 .text（历史调用点零改动）。
    const MOperand& mem_mop(VReg r) const {
        if (r < static_cast<VReg>(vreg_reg_.size()) && !vreg_reg_[r].empty())
            return vreg_reg_[r];
        if (r < static_cast<VReg>(vreg_mem_.size())) return vreg_mem_[r];
        static const MOperand kFallback = op_mem("%rsp", 0);
        return kFallback;
    }
    const MOperand& smem_mop(int slot) const {
        if (slot >= 0 && slot < static_cast<int>(smem_cache_.size()))
            return smem_cache_[static_cast<size_t>(slot)];
        static const MOperand kFallback = op_mem("%rsp", 0);
        return kFallback;
    }
    const MOperand& gen_smem_mop(int slot) const {
        if (slot >= 0 && slot < static_cast<int>(gen_smem_cache_.size()))
            return gen_smem_cache_[static_cast<size_t>(slot)];
        static const MOperand kFallback = op_mem("%r15", 0);
        return kFallback;
    }
    const std::string& mem(VReg r) const { return mem_mop(r).text; }
    const std::string& smem(int slot) const { return smem_mop(slot).text; }
    const std::string& gen_smem(int slot) const { return gen_smem_mop(slot).text; }

    // 变量的现在位置：寄存器（提升过）或栈槽。结构化版本。
    const MOperand& slot_mop(int slot) const override {
        if (gen_) return gen_smem_mop(slot);
        if (slot >= 0 && slot < static_cast<int>(ra_.slot_reg.size()) && !ra_.slot_reg[slot].empty())
            return ra_.slot_reg[slot];
        return smem_mop(slot);
    }
    const std::string& slot_op(int slot) const { return slot_mop(slot).text; }
    // 读一个 vreg 的操作数。块内别名优先（别名指向的就是它的当前值），
    // 其次是可以直接当立即数用的常量，最后退回它自己的栈槽。
    const MOperand& src_mop(VReg r) const override {
        static const MOperand kZero = op_imm(0);
        if (r == kNoReg) return kZero;
        auto it = alias_.find(r);
        if (it != alias_.end()) return it->second;
        auto ct = const_val_.find(r);
        if (ct != const_val_.end()) return ct->second;
        auto gi = global_imm_.find(r);
        if (gi != global_imm_.end()) return gi->second;
        return mem_mop(r);
    }
    const std::string& src_op(VReg r) const { return src_mop(r).text; }

    // 把 src 搬进 dst 操作数。两个都是内存时 x86 没有 mem-to-mem，
    // 中间过一下 rax。
    static bool is_mem_operand(const std::string& s);

    void move_to(const std::string& dst, const std::string& src);

    // 结构化搬运：操作数单元直发。mem-to-mem 判定与字符串版同口径
    // （非 Reg 非 Imm 即「内存类」，含符号/标签兜底）。
    static bool is_mem_mop(const MOperand& o);
    void move_to(const MOperand& dst, const MOperand& src);

    // 浮点搬运。寄存器之间用 movapd 做整 128 位拷贝：movsd 只写低 64 位，
    // 硬件得把它和目的寄存器的旧值合并，于是凭空多出一条对旧值的依赖
    // （紧挨着复用一个临时寄存器时可能明显变慢）。访存仍用 movsd。
    void move_fp(const std::string& dst, const std::string& src);

    // 结构化浮点搬运（与字符串版同口径：%xmm 寄存器判据）。
    void move_fp(const MOperand& dst, const MOperand& src);

    // 本指令 dst 的落点。
    const MOperand& dst_mop(VReg r) const;
    const std::string& dst_op(VReg r) const { return dst_mop(r).text; }

    // 字符串字面量 idx 如果是「单字节 ASCII 的 str」，返回该字节值（0..127），
    // 否则 -1。这类字面量在生成代码里直接取运行时驻留表 pa_char_tab 的对象，
    // 于是和 s[i] 的结果是同一个指针（见 gen_inst 的 ConstStr 分支）。
    int char_tab_literal(int idx) const;

    TypePtr ty_of(VReg r) const override;

    bool is_fp(VReg r) const { return ir_is_float(ty_of(r)); }
    // ── 函数 ────────────────────────────────────────────────────
    std::vector<ArgLoc> classify(const std::vector<TypePtr>& types) const;

    void emit_function(const IRFunction& f, int fi);
    // 统一调用约定的 shim 入口（见 ir/types.h 的 closure_needs_shim）。
    void emit_closure_shim(const IRFunction& f, int fi);

    // O2：只给可能「读在写之前」的槽做序言清零。
    std::vector<bool> slots_needing_zero() const;

    // ── 基本块 ──────────────────────────────────────────────────
    //
    // 别名只在块内有效，块边界一律清空——跨块的值永远走栈槽，所以
    // 不需要在块之间传播任何状态，也就不可能读到过期的寄存器。
    bool can_fuse(const Inst& in, const Inst& nx) const;

    // 浮点二元结果只被紧邻的下一条浮点二元指令使用时，先留在 %xmm1，
    // 省掉「结果 → 临时槽 → 再读回」的一条存储和一条装载。
    bool can_fuse_global(const Inst& in, const Inst& nx) const;
    bool can_fuse_fp_temp(const Inst& in, const Inst& nx) const;

        bool can_fuse_branch(const Inst& in, const Inst& nx) const;

    // 整数 Bin 结果只被紧随的 Cmp 使用、且该 Cmp 与 Branch 融合时，让 Bin
    // 把结果留在 %rax，并把别名传播给 Cmp —— 省掉「Bin 结果 → 临时槽 →
    // cmpq $0,槽」的存储与内存读。判据纯 IR 结构（相邻三指令 + 单次使用）。
    bool can_fuse_bin_cmp_branch(const Inst& a, const Inst& b, const Inst& c) const;

    // 整数 Bin 结果只被紧邻的下一条整数 Bin 使用时，先留在 %rax（或复用
    // 目标寄存器），省掉「结果 → 临时槽 → 再读回」的 store/load。
    bool can_fuse_int_bin_temp(const Inst& in, const Inst& nx) const;
    // 单次使用的整数 Bin 结果被紧邻的调用实参 / return 消费：留在 %rax，
    // 跳过「临时槽 store + load」的往返（fib 这类递归每个调用省 2 条）。
    bool can_fuse_bin_value_use(const Inst& in, const Inst& nx) const;

    // 比较结果只用来做分支时，直接发条件跳转：`setl` + `movzbq` +
    // 一次内存往返 + `cmpq $0` 全部省掉。循环条件走的就是这条路。
    void gen_cmp_branch(const Inst& in, const Inst& br);

    // 浮点比较 + 分支：ucomisd 直接吃标志位，NaN 走 jp 排除。
    void gen_cmp_branch_fp(const Inst& in, const Inst& br);

        void gen_block(const Block& b);
    bool try_forward_global_across_rc(const Block& b, size_t at, size_t& load_pos,
                                      bool& forward_to_receiver);

        bool single_use_in_block(VReg v) const;

        bool is_deferable_const(const Inst& in) const;

        void invalidate_slot(int slot);

    // ── 指令 ────────────────────────────────────────────────────
    void gen_inst(const Inst& in);

    // x = y 这种加载能不能省掉，看两件事：
    //   1. 变量 x 在寄存器里（否则读出来就得写回一个栈槽，省不了）
    //   2. 这个临时值只被读一次，且读的时候中间没有对 x 的赋值
    // 都满足时，临时值直接指向 x 的寄存器，整条指令消失。
    void gen_load_slot(const Inst& in);

        void gen_const_int(const Inst& in);

        // P3 big：永生静态 BigInt 字面量（docs/numeric-types-design.md §1/§4）。
        void gen_const_big(const Inst& in);

        void gen_const_float(const Inst& in);

        void gen_bin(const Inst& in);

        void gen_bin_int(const Inst& in);

        // P1 定宽整数（backend/int_fixed.cpp）：按宽度回绕、u64 用无符号指令。
        void gen_bin_int_fixed(const Inst& in);
        void norm_int_rax(const TypePtr& t);  // 把 %rax 正常化到 t 的规范值
        void gen_udiv64(const Inst& in);      // 商在 %rax、余数在 %rdx

    // idiv 在除数为 0、以及 INT64_MIN / -1 时都会触发 #DE，
    // 那个信号没法带任何上下文。提前挡掉，给一条能看懂的消息。
    void gen_div_guard(bool check_overflow = true);
    // RC 记账（inc/dec/mark/cleanup）的内联快路径。返回 true 表示指令已发完。
    // 慢路径落回 pa_rc_*_slow，且会调运行时 —— 必须落盘提升过的浮点槽。
    bool gen_rc_inline(const Inst& in, RtFn fn);
    // 带全局读的版本：冷路永远重新装载，热路从已发布的操作数转发。
    void gen_rc_dec(const Inst& in, const std::string& src = {},
                    const std::string& dst = {}, int global = -1);
    // 常数除数的除法：能化简就生成「魔数乘法 + 符号修正」，结果留在 %rax
    // （mode 1/2 还会在 %rdx 留下余数），返回 true 表示已生成。
    // 除数为 0 / -1 / INT64_MIN 或不是编译期常量时返回 false，走 idivq。
    bool gen_div_const(const Inst& in, int mode);
    void gen_div_const_emit(int64_t d, int mode, bool a_nonneg);

    // x86 的 idiv 向零取整，Python 的 // 向下取整，余数符号也不同。
    // 差 1 的修正必须做对，否则负数的 // 和 % 全是错的。
    void gen_floordiv(const Inst& in);

    // Python 的 % 结果与除数同号（-7 % 3 == 2），x86 的余数与被除数同号，
    // 差一个除数，需要补回来。
    void gen_mod(const Inst& in);

        void gen_bin_fp(const Inst& in);

        void gen_un(const Inst& in);

    // 把一个值算成 0/1 放进 %al。浮点的 NaN 视为真（Python 语义），
    // 所以用 setne + setp 的组合。
    void emit_truth(VReg r, const char* byte_reg);

        void gen_cmp(const Inst& in);

        void gen_cmp_fp(const Inst& in);

        void gen_convert(const Inst& in);

        void gen_make_list(const Inst& in);
    void gen_tuple_inline(const Inst& in);
    void gen_append_inline(const Inst& in);
    void gen_str_index_inline(const Inst& in);

        void gen_index_load(const Inst& in);

    void gen_index_unchecked_load(const Inst& in);

    void gen_index_store(const Inst& in);
    void gen_index_store_unchecked(const Inst& in);

    // ── 对象 ────────────────────────────────────────────────────
    // 对象布局：+0 是 vtable 指针，字段从 +8 开始，每个 8 字节。
    // 属性访问因此是一条固定偏移的 movq，没有查表、没有哈希。
    void gen_obj_new(const Inst& in);
    void gen_struct_new(const Inst& in);
    void gen_struct_copy(const Inst& in);
        void gen_field_load(const Inst& in);
        void gen_field_store(const Inst& in);
    // 虚调用：self 固定占第一个整数参数（%rdi），其余实参照常分类。
    // 函数地址在装参数之前取到 %r11 —— %r11 不在参数序列里，
    // 装参数的过程不会碰它。
    void gen_vcall(const Inst& in, bool keep_int_result = false, int direct_fn = -1);
    bool gen_guarded_vcall(const Inst& in, bool keep_int_result = false);
    bool gen_guarded_vcall_direct(const Inst& in, bool keep_int_result, int base, size_t slot);
    bool try_fuse_guarded_vcall_add(const Block& b, size_t pos);
    void gen_vcall_static(const Inst& in);
    void gen_super_vcall(const Inst& in);
    void gen_call_direct(const Inst& in);
    void gen_call_ptr(const Inst& in);
        void gen_branch(const Inst& in);
        void gen_return(const Inst& in);
    // ── 调用 ────────────────────────────────────────────────────
    void gen_call(const Inst& in);
    void gen_call_extern(const Inst& in);
    void gen_load_extern(const Inst& in);
    void gen_store_extern(const Inst& in);
    void gen_ptr_load(const Inst& in);
    void gen_ptr_store(const Inst& in);
        void gen_setjmp(const Inst& in);
        void gen_call_rt(const Inst& in);
        // 运行期调用的实参装填：按实参形态分组，实现在 backend/call_rt.cpp。
        void gen_rt_args(const Inst& in, RtFn fn);
        void gen_rt_none(const Inst& in);
        void gen_rt_args_n(const Inst& in, int n);
        void gen_rt_i1(const Inst& in);
        void gen_rt_i2(const Inst& in);
        void gen_rt_i3(const Inst& in);
        void gen_rt_i4(const Inst& in);
        void gen_rt_i5(const Inst& in);
        void gen_rt_i6(const Inst& in);
        void gen_rt_f1(const Inst& in);
        void gen_rt_f2(const Inst& in);
        void gen_rt_f3(const Inst& in);
        void gen_rt_i1f1(const Inst& in);
        void gen_rt_i1f2(const Inst& in);
        void gen_rt_i2f1(const Inst& in);  // (int, int, double)
        void gen_rt_i1f3(const Inst& in);
        void gen_rt_f1i1(const Inst& in);
        void gen_rt_i4opt5(const Inst& in);
        void gen_rt_reduce_f(const Inst& in);
        void gen_rt_subprocess_run(const Inst& in);
