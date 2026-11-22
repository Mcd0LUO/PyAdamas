// codegen_tag.h —— P3 shadow tag / 直调 tag ABI / 零开销异常（类体片段）
//
// **本文件是 class AsmGen 的类体片段**：由 codegen_impl.h 在 class 体内
// #include，内容就是 AsmGen 的成员声明，不单独编译、没有 include guard。
// 不要在文件里写 namespace / 文件级 #include —— 那会出现在类体中间，编译失败。
//
// 从 codegen_impl.h 按原有分组注释整段切出（纯文本移动，无逻辑改动）。
// 实现见 codegen_tag.cpp（shadow tag / tag ABI）与 eh_emit.cpp（零开销异常）。

    // ── P3 shadow tag（0.327，docs/p3-tagged-union-design.md）─────────
    //
    // unknown 值仍是 8B i64（float 仍装箱），平行 tag 字跟值走：tag 是
    // RC/分派快路径的判据，UNK(0) 一律回退探针 —— 正确性不依赖传播完备。
    // 门控 tags_enabled_ = 模块含 AnyArithT/AnyCmpT/AnyNegT（封闭活跃域）；
    // 门控关的模块（含 26 个热基准）不进任何 tag 分支，asm 逐字节不变。
    // tag 编码与静态类型映射见 tag_defs.h（含 P2 的 NONE/BOOL/堆子类扩展，
    // 以及堆 ⇔ 3..5 或 >=8 的判定口径）。
    bool tags_enabled_ = false;
    bool type_tags_ = false;              // PYADAMAS_TYPE_TAGS（默认关）
    int n_tag_words_ = 0;                 // 帧内平行 tag 区字数
    std::vector<int> vreg_tag_;           // vreg -> tag 字号（-1 无）
    std::vector<int> slot_tag_;           // 命名槽 -> tag 字号（-1 无）
    std::vector<MOperand> vreg_tag_mem_;
    std::vector<MOperand> slot_tag_mem_;
    // tag 区在 vreg 栈槽区之上、帧记录之下（rec_off 按 frame_.locals 自动让位）。
    int tag_off(int w) const;
    static bool is_unk_ty(const TypePtr& t);
    bool unk_vreg(VReg r) const;
    bool unk_slot(int s) const;
    // 全局 tag 健全性：只有「别的模块写不到」的 unknown 全局才配 tag 字。
    // 外部全局（存储属对方）与非入口模块的导出全局（可能被 import 方写入，
    // 对方不维护本模块 tag）一律 UNK 回退。入口模块 __entry__ 不可被 import。
    bool global_tag_eligible(int gi) const;
    std::string global_tag_label(int gi) const;
    // 静态类型导出的 tag（定义在 tag_defs.h；type_tags_ 决定 P2 扩展）。
    int64_t static_tag_of(const TypePtr& t) const;
    bool vreg_tagged(VReg r) const;
    bool slot_tagged(int s) const;
    // tag 操作数：结构化单元优先，字符串接口委托 .text。
    const MOperand& tag_mop(VReg r) const;
    const MOperand& slot_tag_mop(int s) const;
    const std::string& tag_mem(VReg r) const { return tag_mop(r).text; }
    const std::string& slot_tag_op(int s) const { return slot_tag_mop(s).text; }
    // vreg 的 tag 源操作数：有 tag 字读字，没有按静态类型导常量。
    std::string tag_src(VReg r) const;
    // 写 dst 的 tag（常量）。
    void emit_tag_imm(VReg r, int64_t t);
    // 调用返回的 unknown dst 落 tag：除已知生产者外一律 UNK（探针回退）。
    void emit_call_dst_tag(const Inst& in);
    // AnyArithT/AnyNegT 的复用匹配（设计 §4）：结果被存回 S 且某实参恰是
    // 同块对 S 的装载、装载到存储之间无对 S 的其它写入 → 1，否则 0。
    int64_t any_reuse_ok(const Inst& in);
    void gen_any_tagged(const Inst& in, RtFn fn);
    void gen_rc_unknown_tagged(const Inst& in, RtFn fn);
    void gen_append_tagged(const Inst& in);
    void gen_index_store_tagged(const Inst& in, bool unchecked);
    void gen_index_load_tag(const Inst& in, bool have_regs);
    void emit_heap_from_tag(VReg v, const std::string& dst_reg);

    // ── P3-1b：直调 tag ABI（0.328，设计 §7 P3-1b）─────────────────
    // 值 ABI 不变（payload 仍按 classify 落位）：unknown 形参的 tag 经
    // 平行 tag 数组传递，调用方把数组指针放 %r11、call 被调的 $pt 入口；
    // 旧入口（闭包/间接/跨模块/vtable）先把 %r11 清零再进同一函数体，
    // 形参 tag 全 UNK 回退探针。unknown 返回经 %rdx 带 tag（旧调用方
    // 忽略 rdx，无兼容问题）。门控关/生成器一律不进。
    static std::string ptag_label(int i);
    // 形参是否在 tag 数组里占一个字（type_tags_ 时按声明类型，否则按槽类型）。
    bool ptag_param(const IRFunction& f, const IRParam& p) const;
    // 本函数 unknown 形参个数（>0 才发双入口；生成器/门控关恒 0）。
    int func_ptag_words() const;
    // CallDirect 的 tag 数组字数（0 = 不走 tag ABI：门控关/跨模块/生成器/
    // 实参数与形参数不一致）。
    int call_ptag_words(const Inst& in) const;
    // 函数是否经 %rdx 返回 tag（unknown 返回 + 门控 + 非生成器）。
    bool func_ret_tag(const IRFunction& f) const;
    // 被调双入口 gadget：旧入口清零 %r11 跳函数体，$pt 入口直通。
    // gadget 不动 rsp（CFI CFA=rsp+8 不变）；LSDA 按标签差解析自动让位。
    void emit_ptag_entry(int fi);
    // 被调序言：从 %r11 tag 数组装载 unknown 形参 tag（0 → 保持 UNK）。
    // %r11 从入口到此只经 push/subq/movq $0（都不碰它）。
    void emit_ptag_param_load();
    // Return 的 %rdx tag（unknown 返回 + 门控 + 非生成器；否则空操作）。
    void emit_ret_tag(const Inst& in);
    // P3-1b：无 tag 来源的 unknown 装载（dict 值/字段）一次探针导 tag。
    void emit_tag_probe(const std::string& payload_op, const std::string& tag_op);
    // 调用方：在出参区栈实参之后写 tag 数组，返回数组的 rsp 偏移。
    int emit_ptag_array(const Inst& in, const std::vector<ArgLoc>& locs);

    // ── 零开销异常（Z2-b2，--zero-cost-exc）────────────────────────
    //
    // 每个函数的 EH 计划在 emit_function 开头按 IR 里的 EhBegin/EhEnd/
    // EhLandingPad 标记**预扫描**出来（标签名也在那时按发射顺序分配好，
    // 发射标记指令时按同一顺序取用）。.cfi_personality / .cfi_lsda 紧跟
    // .cfi_startproc 发射；.gcc_except_table 与 DW.ref 指针槽等所有函数都
    // 发完后在 gen() 尾部统一发。计划为空 = 老 setjmp 路径，一个字节不发。
    // 一个函数的所有保护区合成一份 LSDA（多条 call-site 记录）。
    // 嵌套 try 时每个 region 各占一条记录，共用同一个 landing pad 分发入口
    // （分发链按 handler 顺序逐个匹配，与老路径一致）。
    struct EhPlanRec {
        std::string func_label;  // FDE 起点（= .cfi_startproc 地址，不是 .LfN）
        struct Range {
            std::string begin;   // 保护区起点标签
            std::string end;     // 保护区终点标签（开区间）
            std::string lp;      // 该区间的 landing pad 标签
        };
        std::vector<Range> ranges;
        int lsda_index = 0;
    };
    std::vector<EhPlanRec> eh_plans_;
    // 当前函数预扫描出来的标签（发射标记指令时按顺序取用）。
    std::vector<std::string> eh_begin_labels_;
    std::vector<std::string> eh_end_labels_;
    std::vector<std::string> eh_lp_labels_;
    size_t eh_begin_seq_ = 0;
    size_t eh_end_seq_ = 0;
    size_t eh_lp_seq_ = 0;
    bool eh_plan_active_ = false;
    // 总开关（--zero-cost-exc）：关闭时 prepare_eh_plan 直接返回，老路径 asm 不变。
    bool zero_cost_exc_ = true;
    bool rc_split_ = false;
    bool ra_opt_ = false;    // RA_OPT：P1 跨调用提升 + P2 只读别名（ra_opt.cpp），默认关
    bool inline_opt_ = false;  // INLINE_OPT：内联/devirt 深化 + 入口对齐修复，默认关
    void prepare_eh_plan(const IRFunction& f, const std::string& fname);  // 预扫描 EH 标记
    void emit_eh_directives_for_current();  // 发 .cfi_personality / .cfi_lsda
    // 发所有 LSDA + DW.ref（在 gen() 尾部、text 都发完之后）。
    void emit_eh_tables();
    void gen_eh_begin();
    void gen_eh_end();
    void gen_eh_landing_pad();
