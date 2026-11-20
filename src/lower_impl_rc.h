// lower_impl_rc.h —— Lowerer 的引用计数成员声明
//
// lower_impl.h 的组成部分（纯代码移动）：在 class Lowerer 体内原位置
// #include 展开，声明顺序与内容逐字不变。不要单独 include。

    // ── 引用计数 ────────────────────────────────────────────────
    //
    // 所有权约定：
    //   槽 / 全局 / 字段 / 列表元素 持有引用；
    //   load / field.get / list.get 只是借用，不增加计数；
    //   每个新堆值产生时带 rc=1 进临时栈，语句边界统一清理。
    // unknown 类型不插桩：机器表示可能是整数，按指针 dec 会崩。

    void rc_inc(VReg v) { call_void(RtFn::RcInc, {v}); }
    void rc_dec(VReg v) { call_void(RtFn::RcDec, {v}); }
    void rc_temp_take(VReg v) { call_void(RtFn::RcTempTake, {v}); }
    // unknown 值可能是整数也可能是堆指针，交给运行时按注册表判断。
    void rc_inc_unknown(VReg v) { call_void(RtFn::RcIncUnknown, {v}); }
    void rc_dec_unknown(VReg v) { call_void(RtFn::RcDecUnknown, {v}); }
    /* 增量标记插入屏障（0.330）：编译期门控 PYADAMAS_GC_BARRIER=1 时对堆指针
       写发射 pa_gc_write_barrier（标灰新值）。默认关 → 生成 asm 逐字不变
       （26 热基准按构造保护）。运行期再由 gc_incr_state==1 决定是否生效。 */
    void gc_barrier(VReg v) {
        static const bool on = []{ const char* e = std::getenv("PYADAMAS_GC_BARRIER");
                                   return e && e[0] == '1'; }();
        if (on) call_void(RtFn::GcWriteBarrier, {v});
    }

    // 把刚产生的堆值的所有权从临时栈转给目标。成功返回 true，调用方
    // 跳过 retain；目标类型和源类型都不是堆时返回 false（无需持有引用）。
    bool move_fresh(VReg v, const TypePtr& dst) {
        if (!st_->fresh_vregs.count(v)) return false;
        TypePtr src = vreg_ty(v);
        if (!ir_is_heap(dst) && !ir_is_heap(src)) return false;
        st_->fresh_vregs.erase(v);
        rc_temp_take(v);
        // take 把引用移出临时栈，净 push 减一：stmt / 循环的 mark/cleanup
        // 消除按净 push 判断，push 后立刻被槽/容器 take 的值（queue_sim 的
        // 元组 append、rest = [] 赋值）不再留下水位脚手架。fresh_vregs 的
        // 每个插入点都配对了 push_temp，计数不会下溢。异常路径不受影响：
        // 被消的 cleanup 本来就不在展开路径上跑，push 与 take 之间抛异常
        // 滞留的临时值由外层水位兜底（try 的 mark_slot landing pad、caller
        // 的语句水位、入口 fn_mark）——与既有循环/推导式消除同款不变量。
        --st_->rc_pushes;
        return true;
    }
    // 槽/字段/全局按「是否可能装着非堆值」选释放方式。
    void release_slot_value(int slot, TypePtr ty, bool maybe_unknown) {
        VReg v = load_slot(slot, ty);
        if (maybe_unknown) rc_dec_unknown(v);
        else rc_dec(v);
    }

    VReg rc_mark() { return call_runtime(RtFn::RcMark, t_int(), {}); }
    void rc_cleanup(VReg mark) { call_void(RtFn::RcCleanup, {mark}); }

    VReg rc_mark_at(int& block, size_t& pos) {
        VReg m = rc_mark();
        block = st_->blk;
        pos = cur().insts.size() - 1;
        return m;
    }

    void push_temp(VReg v) {
        ++st_->rc_pushes;
        call_void(RtFn::RcTemp, {v});
    }

    // unknown 域的动态结果：可能是 int 也可能是堆盒，只能走保守的
    // RcTempUnknown（运行时先按注册表确认是堆对象）。**不进 fresh_vregs**：
    // 容器构造的 take 快路径假设值一定是堆，int 会被当指针解引用。
    void push_temp_unknown(VReg v) {
        ++st_->rc_pushes;
        call_void(RtFn::RcTempUnknown, {v});
    }

    // 确定是新分配的堆对象、但静态类型是 unknown（float 装箱盒）：
    // 与 def_heap 同口径，容器构造可以 take 走这份引用。
    void push_temp_any_heap(VReg v) {
        ++st_->rc_pushes;
        call_void(RtFn::RcTempUnknown, {v});
        st_->fresh_vregs.insert(v);
    }

    void erase_mark(int block, size_t pos, VReg mark) {
        auto& ins = st_->fn->blocks[block].insts;
        if (pos < ins.size() && ins[pos].op == IrOp::CallRt &&
            static_cast<RtFn>(ins[pos].imm) == RtFn::RcMark && ins[pos].dst == mark)
            ins.erase(ins.begin() + static_cast<std::ptrdiff_t>(pos));
    }

    void erase_cleanup(int block, VReg mark) {
        auto& ins = st_->fn->blocks[block].insts;
        std::erase_if(ins, [&](const Inst& in) {
            return in.op == IrOp::CallRt && static_cast<RtFn>(in.imm) == RtFn::RcCleanup &&
                   in.args.size() == 1 && in.args[0] == mark;
        });
    }

    // 产生一个堆值：把它交给临时栈，调用方不需要再管这个初始引用。
    VReg def_heap(TypePtr ty, Inst in) {
        // P3-1b（0.328）：用户调用（Call/CallDirect/VCall*）的 unknown 返回
        // 也带一份调用方所有的引用（emit_return 对 unknown 返回 RcIncUnknown /
        // fresh take），必须同样登记进临时栈，否则直接消费（print/算术）时
        // 那份引用无人释放 —— 0.325 基线已存在的「print(object 调用结果)」
        // 所有权洞（j12 str 104B / m5 float 盒 72B / 144B 双盒形态）。
        // 只限用户调用 op：CallRt/Bin/IndexLoad 等 unknown 结果是借用或裸
        // 机器字，登记会误 dec。不进 fresh_vregs：值可能是整数，容器 take
        // 快路径假设一定是堆（与 push_temp_unknown 同口径）。
        const bool user_call =
            in.op == IrOp::Call || in.op == IrOp::CallDirect ||
            in.op == IrOp::VCall || in.op == IrOp::VCallStatic ||
            in.op == IrOp::SuperVCall;
        VReg v = def(ty ? ty : t_unknown(), std::move(in));
        if (ir_is_heap(ty)) {
            push_temp(v);
            st_->fresh_vregs.insert(v);
        } else if (user_call && is_unknown(ty)) {
            push_temp_unknown(v);
        }
        return v;
    }

    // 运行时按机器字返回的值（生成器 next/send）静态类型只能是 unknown；
    // 用 mov 补一个静态类型，后续 store_slot 才能按堆类型 retain。
    VReg retype(VReg v, TypePtr ty) {
        if (!ty || type_eq(vreg_ty(v), ty)) return v;
        Inst mv;
        mv.op = IrOp::Mov;
        mv.a = v;
        mv.ty = ty;
        return def(ty, std::move(mv));
    }

    // 队列元素的运行时类别：0 裸值 / 1 引用。unknown 保守按引用。
    static int queue_kind(const TypePtr& t) {
        if (!t || t->kind != TyKind::AsyncQueue || t->sub.empty()) return 1;
        TypePtr e = t->sub[0];
        if (!e || e->kind == TyKind::Unknown) return 1;
        return ir_is_heap(e) ? 1 : 0;
    }

    // 列表元素所有权类别，必须和 codegen 传给 pa_list_new 的值一致。
    // 3 = 未定：类型里看不出元素种类（裸 list / []），空容器先记 3，第一次
    // 写入时由 pa_list_prep 按运行时值定型。
    static int list_kind(const TypePtr& t);

    // 容器一开始就有元素（或由运行时自行填充）时不能停在「未定」3：
    // 直接降级为 2，保持和原来一致的保守所有权语义。
    static int list_kind_filled(const TypePtr& t) {
        int k = list_kind(t);
        return k == 3 ? 2 : k;
    }

    // kind 1/2 且源类型已知是堆，写入前 retain。kind 2 只 retain 不 release
    //（类型看不见，销毁时不敢按元素 dec），代价是泄漏。源类型是 unknown
    // 时不能直接 rc_inc（可能是整数），走注册表保守判断。
    void retain_elem(VReg v, int kind) {
        if (kind == 0) return;
        TypePtr vt = vreg_ty(v);
        if (ir_is_heap(vt)) {
            rc_inc(v);
            return;
        }
        if (!vt || vt->kind == TyKind::Unknown) rc_inc_unknown(v);
    }

    // 列表元素写入前的所有权交接。容器未定型（3）、静态种类与值冲突、或值
    // 是 unknown 时交给运行时的 pa_list_prep 定型并 retain；其余情况按静态
    // 种类直接 retain，省掉一次调用。
    void store_list_elem(VReg l, const TypePtr& lt, VReg v, bool owned) {
        int lk = list_kind(lt);
        int vk = own_kind(vreg_ty(v));
        if (lk == 3 || (lk != 2 && vk == 2) || (lk == 0 && vk == 1) || (lk == 1 && vk == 0)) {
            call_void(RtFn::ListPrep, {l, v, const_int(vk), const_int(owned ? 1 : 0)});
            return;
        }
        if (!owned) retain_elem(v, lk);
    }

    // 哈希/比较类别：0 int/bool，1 str，2 float，3 其他（按机器字比较）。
    static int key_kind(const TypePtr& t);

    // 所有权类别：0 非堆，1 堆，2 unknown（保守 retain，注册表释放）。
    static int own_kind(const TypePtr& t) {
        if (ir_is_heap(t)) return 1;
        if (!t || t->kind == TyKind::Unknown) return 2;
        return 0;
    }

    // 排序比较器的种类：0 int/bool、1 float、2 str、4 元组（desc 字典序）、
    // 5 u64（无符号比较）、其它按机器字。0.312 前元组落 3：比堆地址≈分配
    // 顺序，sort 是 no-op。窄定宽（i8/u8/…/u32）槽内值已是规范符号/零扩展，
    // 有符号机器字比较与按宽度比较逐例等价，仍落 0；只有 u64 的 ≥2^63 值
    // 位模式是负数，必须走无符号比较器（P1 收尾）。
    static int sort_kind(const TypePtr& t) {
        if (!t) return 3;
        if (t->kind == TyKind::U64) return 5;
        if (is_int_like(t)) return 0;  // 其余定宽整数与 int 同一排序域
        if (t->kind == TyKind::Float) return 1;
        if (t->kind == TyKind::Str) return 2;
        if (t->kind == TyKind::Tuple || t->kind == TyKind::List) return 4;
        return 3;
    }

    // 排序描述符（仅 kind 4 有意义，标量种类给空串，运行时不读）。
    // 生成失败（病态深度/长度）退 "x"：机器字比较，即修复前行为。
    std::string sort_desc(const TypePtr& t) {
        if (sort_kind(t) != 4) return "";
        std::string d;
        if (!append_sort_desc(t, d, 0)) return "x";
        return d;
    }

    // 内置序列构造（list/sorted/enumerate/...）的入参规整成运行时列表：
    // str → 字符列表，dict/set → 键列表，list/tuple 原样，生成器顺序消费。
    VReg seq_as_list(Expr* e, TypePtr& elem_out);
    // 生成器物化成 list：惰性值没有 len/下标，只能顺序消费一次。
    VReg gen_to_list(VReg gen, TypePtr gen_ty);
    // 生成器流式迭代：每个元素执行一次 body(ev, elem_ty, cond_blk, end_blk)。
    // ev 是借用引用；body 里跳 cond_blk 继续下一轮、跳 end_blk 提前结束。
    void emit_gen_for_each(VReg gen, TypePtr gen_ty,
                           const std::function<void(VReg, TypePtr, int, int)>& body);

    // 槽/全局/字段按**目标类型**决定是否 retain：出口按目标类型释放，
    // 只按源类型加引用会在「unknown 值写进带标注的堆槽」时失衡
    // （源类型未知就漏掉 inc，出口却照常 dec）。unknown 源用运行时
    // 保守 inc，整数在这里是安全的 no-op。
    void retain_for_store(const TypePtr& dst, VReg v);
    // P3-1b：object/Any 标注局部槽登记禁升级（见 lower_impl_util.cpp）。
    void mark_ann_unknown_slot(AnnAssign& a);

    void store_slot(int slot, VReg v);

    void store_global(int gi, VReg v);

    void store_field(TypePtr fty, VReg obj, int off, VReg v);

    void inc_params() {
        for (auto& p : st_->fn->params) {
            if (ir_is_heap(p.ty)) {
                rc_inc(load_slot(p.slot, p.ty));
            } else if (!p.ty || p.ty->kind == TyKind::Unknown) {
                // unknown 形参也归形参槽所有：否则出口无法区分「槽里是
                // 调用方借来的实参」和「函数体写入并 retain 过的值」，
                // release_slots 只能整类跳过 unknown 槽（0.323 之前的
                // 口径），导致函数体写入的引用泄漏。保守 inc 的代价是
                // 每次调用一对注册表探测，标量实参在探针里是空操作。
                rc_inc_unknown(load_slot(p.slot, p.ty));
                st_->fn->slot_owned[static_cast<size_t>(p.slot)] = true;
            }
        }
    }

    // 函数出口释放所有堆槽：形参在入口 retain 过，局部变量在写入时 retain
    // 过。返回值必须先 rc_inc，再走到这里，否则槽一释放就成悬空指针。
    // 释放后把槽清零：内联会把槽留在调用方帧里跨多次执行复用，残留的
    // 悬空指针会让下一次赋值的「load 旧值 + dec」二次释放。
    void release_slots();

    // 只读形参不需要「入口 retain + 出口 release」：实参在调用期间由
    // 调用方持有，函数体既没覆盖形参槽、也没把它存进别处时，这对 RC
    // 纯属多余。方法调用的 self 绝大多数是这种形态，省掉的是每次
    // 调用两条运行时调用。判定在函数降级完成后做：
    //   - 入口 retain 是入口块开头按形参顺序排的两条指令（load + rc_inc）；
    //   - 出口 release 只由 release_slots 发出，形态是 load(param) + rc_dec，
    //     只读形参意味着函数体里不会覆盖它，所以这种形态只可能是出口释放。
    // 不记录指令下标：降低过程中语句/循环的 mark 擦除会让下标失效。
    void finalize_param_borrows();
    // 只读借用槽细化（门控 PYADAMAS_BORROW_OPT，默认关）：把「每轮从
    // 稳定容器借用、只被读取」的槽改成 borrow，免每轮 inc/dec 与出口
    // release。判定与安全论证见 lower_borrow.cpp / docs/borrow-loop-design.md。
    void finalize_borrow_slots();

    void release_globals();

    void emit_return(VReg v, TypePtr vty, bool already_owned = false);
    // 把返回值变成 caller 拥有的引用（新值从临时栈取走，借用值补引用）。
    // **必须在跑清理之前调用**，否则清理可能先把返回值本身释放掉。
    VReg own_return_value(VReg v, TypePtr vty);
