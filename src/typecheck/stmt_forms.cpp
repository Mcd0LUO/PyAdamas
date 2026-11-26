// typecheck/stmt_forms.cpp —— stmt 按语句形态拆分
#include "driver/build.h"
#include "typecheck/typecheck.h"

#include <algorithm>
#include <filesystem>
#include <set>

#include "ir/module.h"

namespace pyadamas {

bool TypeChecker::literal_int_value(const Expr* e, int64_t& out) {
    if (!e) return false;
    if (e->kind == ExprKind::Constant) {
        const auto& c = static_cast<const Constant&>(*e);
        if (c.ck == Constant::CK::Int) {
            // u64 大字面量的位模式在 int64 里是负数，不能按普通字面量取值
            // （否则 -18446744073709551615 会被算成 1）。它由
            // accept_literal_into 的 u64 分支单独处理。
            if (c.u64_lit) return false;
            out = c.i;
            return true;
        }
        if (c.ck == Constant::CK::Bool) {
            out = c.b ? 1 : 0;
            return true;
        }
        return false;
    }
    if (e->kind == ExprKind::UnaryOp) {
        const auto& u = static_cast<const UnaryOp&>(*e);
        if (u.op != Tok::MINUS && u.op != Tok::PLUS) return false;
        int64_t v = 0;
        if (!literal_int_value(u.operand.get(), v)) return false;
        // -INT64_MIN 在 int64 里不可表示（C++ 的 -INT64_MIN 是 UB）。
        if (u.op == Tok::MINUS) {
            if (v == INT64_MIN) return false;
            out = -v;
        } else {
            out = v;
        }
        return true;
    }
    return false;
}

bool TypeChecker::accept_literal_into(const TypePtr& want, Expr* value) {
    if (!want || (!is_fixed_int(want) && want->kind != TyKind::Big) || !value) return false;
    // u64 大字面量（>INT64_MAX 的十进制，位模式存 i）：只有 U64 目标收得下；
    // 其它定宽目标按无符号值报超范围。不走 literal_int_value（位模式是负数）。
    if (auto* c = dynamic_cast<Constant*>(value);
        c && c->ck == Constant::CK::Int && c->u64_lit) {
        u64_lit_ok_.insert(value);  // 已处理（收下或已报错），收尾不再叠加
        if (want->kind == TyKind::U64) {
            expr_types_[value] = want;
            return true;
        }
        if (want->kind == TyKind::Big) {
            // P3：big 目标消费 u64 大字面量——改写成 CK::Big 十进制串
            //（位模式按 i64 转 big 会带上负号，必须按无符号值转写）。
            c->ck = Constant::CK::Big;
            c->s = std::to_string(static_cast<uint64_t>(c->i));
            c->u64_lit = false;
            expr_types_[value] = want;
            return true;
        }
        int64_t lo = 0, hi = 0;
        int_range_of(want->kind, lo, hi);
        err(value->line, value->col,
            std::format("literal {} is out of range for '{}' (allowed {}..{})",
                        static_cast<uint64_t>(c->i), type_name(want), lo, hi));
        return true;
    }
    if (is_fixed_int(want)) {
        const Expr* magnitude = value;
        while (auto* u = dynamic_cast<const UnaryOp*>(magnitude)) {
            if (u->op != Tok::PLUS && u->op != Tok::MINUS) break;
            magnitude = u->operand.get();
        }
        if (auto* c = dynamic_cast<const Constant*>(magnitude); c && c->ck == Constant::CK::Big) {
            err(value->line, value->col,
                std::format("integer literal too large for int64; does not fit '{}'", type_name(want)));
            return true;
        }
    }
    // float/bool 字面量的处理：bool 已是 int 子类型，assignable 放行；
    // float 字面量窄化到定宽整数不在 v1 隐式允许范围（要 cast）。
    int64_t v = 0;
    if (!literal_int_value(value, v)) return false;
    if (!int_literal_fits(want, v)) {
        int64_t lo = 0, hi = 0;
        int_range_of(want->kind, lo, hi);
        err(value->line, value->col,
            std::format("literal {} is out of range for '{}' (allowed {}..{})", v,
                        type_name(want), lo, hi));
        return true;  // 已经报错，调用方不要再叠加通用消息
    }
    // **把字面量的静态类型改写成目标类型。**
    // 这样后续 assignable() 看到的是 i8→i8（不是 int→i8），降级层
    // 生成常量时也按 i8 定型，值域已检查过，正常化是恒等。
    expr_types_[value] = want;
    return true;
}

bool TypeChecker::coerce_container_literal(const TypePtr& want, Expr* value) {
    if (!want || !value) return false;
    // 收集 (元素表达式, 目标元素类型) 位置；目标不是定宽整数的位置不收。
    std::vector<std::pair<Expr*, TypePtr>> slots;
    if ((want->kind == TyKind::List && value->kind == ExprKind::List) ||
        (want->kind == TyKind::Set && value->kind == ExprKind::Set)) {
        if (want->sub.empty() || !is_fixed_int(want->sub[0])) return false;
        auto& elts = value->kind == ExprKind::List
                         ? static_cast<ListExpr*>(value)->elts
                         : static_cast<SetExpr*>(value)->elts;
        for (auto& el : elts) slots.emplace_back(el.get(), want->sub[0]);
    } else if (want->kind == TyKind::Dict && value->kind == ExprKind::Dict) {
        auto& d = static_cast<DictExpr&>(*value);
        const bool kf = !want->sub.empty() && is_fixed_int(want->sub[0]);
        const bool vf = want->sub.size() > 1 && is_fixed_int(want->sub[1]);
        if (!kf && !vf) return false;
        for (size_t i = 0; i < d.values.size(); ++i) {
            if (kf && d.keys[i]) slots.emplace_back(d.keys[i].get(), want->sub[0]);
            if (vf) slots.emplace_back(d.values[i].get(), want->sub[1]);
        }
    } else {
        return false;
    }
    if (slots.empty()) return false;  // 空字面量走既有的洞回填路径
    // 先整体判定：所有目标位置都必须是字面量（含 u64 大字面量），否则
    // 一个都不改写——避免「半改写」状态（部分元素定了型、容器没定）。
    for (auto& [el, ty] : slots) {
        if (auto* c = dynamic_cast<const Constant*>(el);
            c && c->ck == Constant::CK::Int && c->u64_lit)
            continue;
        int64_t v = 0;
        if (!literal_int_value(el, v)) return false;
    }
    for (auto& [el, ty] : slots) accept_literal_into(ty, el);
    expr_types_[value] = want;
    return true;
}

void TypeChecker::note_u64_literal(const Expr* e) {
    u64_lit_pending_.insert(e);
}

void TypeChecker::consume_u64_literal(const TypePtr& rt, Expr* e) {
    if (!rt || (rt->kind != TyKind::U64 && rt->kind != TyKind::Big) || !e) return;
    auto* c = dynamic_cast<Constant*>(e);
    if (c && c->ck == Constant::CK::Int && c->u64_lit) {
        if (rt->kind == TyKind::Big) {
            // P3：提升到 Big 的运算/比较：u64 大字面量改写成 CK::Big
            // 十进制串（按无符号值，位模式直转会带负号）。
            c->ck = Constant::CK::Big;
            c->s = std::to_string(static_cast<uint64_t>(c->i));
            c->u64_lit = false;
            expr_types_[e] = t_big();
        } else {
            // 提升到 U64 的运算/比较：字面量按 U64 定型，降级直接发无符号
            // 位模式常量（两侧转换到提升类型是恒等，回绕语义不变）。
            expr_types_[e] = t_u64();
        }
        u64_lit_ok_.insert(e);
    }
}

void TypeChecker::check_u64_literals() {
    // 未被 u64 上下文消费的大字面量：默认 int 装不下，照旧报错。
    // 按行列排序去重，保证诊断顺序确定（重查会重复登记同一节点）。
    std::vector<const Expr*> rest;
    for (const Expr* e : u64_lit_pending_)
        if (!u64_lit_ok_.count(e)) rest.push_back(e);
    std::sort(rest.begin(), rest.end(), [](const Expr* a, const Expr* b) {
        return a->line != b->line ? a->line < b->line : a->col < b->col;
    });
    rest.erase(std::unique(rest.begin(), rest.end()), rest.end());
    for (const Expr* e : rest)
        err(e->line, e->col, "integer literal too large for int64");
}

void TypeChecker::stmt_assign(Assign& a) {
        // 0.575：装饰器脱糖产生的合成赋值（f = __pyadamas$dec_N(f)）——
        // 类型检查侧对装饰器的处理保持原样，这里只把 RHS 过一遍（求副作用），
        // 不参与「标注/from_def 冲突」那套规则。
        if (a.value && a.value->kind == ExprKind::Call) {
            auto& c0 = static_cast<Call&>(*a.value);
            if (auto* fn0 = dynamic_cast<Name*>(c0.func.get());
                fn0 && fn0->id.starts_with("__pyadamas$dec_")) {
                expr(*a.value);
                for (auto& t : a.targets) expr(*t);
                return;
            }
        }
        // d = globals() 之后 d[k] = v 等价于模块级动态注入，记下别名
        if (a.value->kind == ExprKind::Call) {
            auto& c = static_cast<Call&>(*a.value);
            auto* fn = dynamic_cast<Name*>(c.func.get());
            if (fn && fn->id == "globals")
                for (auto& t : a.targets)
                    if (auto* tn = dynamic_cast<Name*>(t.get()))
                        globals_aliases_.insert(tn->id);
        }
        auto vt = expr(*a.value);
        for (auto& t : a.targets) {
            // 定宽目标 + 字面量：范围内隐式 coerce（超范围报编译错误）。
            TypePtr want;
            if (auto* tn = dynamic_cast<Name*>(t.get())) {
                if (auto* sym = lookup(tn->id); sym && sym->ty) {
                    if (is_fixed_int(sym->ty)) want = sym->ty;
                    else if (sym->ty->kind == TyKind::List || sym->ty->kind == TyKind::Set ||
                             sym->ty->kind == TyKind::Dict)
                        want = sym->ty;  // 容器字面量 coerce（元素定宽才生效）
                }
            } else if (auto* sub = dynamic_cast<Subscript*>(t.get())) {
                // 定宽容器元素的下标写入：l[0] = 5 / d["k"] = 5 与 append 同口径。
                // 只看 Name 接收者：复杂接收者不重复求值（副作用/重复报错）。
                if (sub->slice && sub->slice->kind != ExprKind::Slice) {
                    if (auto* rn = dynamic_cast<Name*>(sub->value.get())) {
                        if (auto* sym = lookup(rn->id); sym && sym->ty) {
                            const TypePtr& ot = sym->ty;
                            if (ot->kind == TyKind::List && !ot->sub.empty() &&
                                is_fixed_int(ot->sub[0]))
                                want = ot->sub[0];
                            else if (ot->kind == TyKind::Dict && ot->sub.size() > 1 &&
                                     is_fixed_int(ot->sub[1]))
                                want = ot->sub[1];
                        }
                    }
                }
            } else if (auto* at = dynamic_cast<Attribute*>(t.get())) {
                // 定宽的类字段：赋值 c.n = 127 与 Name 目标同口径（范围内隐式）。
                // 模块/cimport 的接收者不能求值：m.x = 1 里的 m 是模块，
                // 求值路径会报「module cannot be used as a value」。
                bool base_mod = false;
                if (auto* bn = dynamic_cast<Name*>(at->value.get())) {
                    base_mod = lookup_module_alias(bn->id) != nullptr ||
                               cmodules_.count(bn->id) > 0;
                    // 定宽 C 变量 + 字面量：与标量赋值同口径 coerce（P1 收尾）。
                    if (base_mod) {
                        if (const CImportDecl* cd = cmodule(bn->id)) {
                            for (auto& cv : cd->vars) {
                                if (cv.py_name != at->attr) continue;
                                TypePtr wt = ckind_type(cv.kind);
                                if (is_fixed_int(wt)) want = wt;
                                break;
                            }
                        }
                    }
                }
                if (!base_mod) {
                    TypePtr rt = expr(*at->value);
                    if (rt && rt->kind == TyKind::Class && rt->cls) {
                        if (const FieldInfo* f = rt->cls->field(at->attr);
                            f && f->ty && (is_fixed_int(f->ty) || f->ty->kind == TyKind::Big))
                            want = f->ty;
                    }
                }
            }
            if (want && (accept_literal_into(want, a.value.get()) ||
                         coerce_container_literal(want, a.value.get()))) {
                assign(*t, want, a.line, a.col);
                continue;
            }
            assign(*t, vt, a.line, a.col);
        }
        return;
    }

void TypeChecker::stmt_aug_assign(AugAssign& a) {
        auto cur_ty = expr(*a.target);
        // list += 可迭代对象：+= 走的是 list.__iadd__，吃任意 iterable，
        // 和 list + tuple 是两回事（后者才非法）
        if (a.op == Tok::PLUSEQUAL && cur_ty && cur_ty->kind == TyKind::List) {
            auto vt = expr(*a.value);
            // 定宽元素 += 字面量列表：逐元素 coerce，与 extend 同口径。
            if (coerce_container_literal(cur_ty, a.value.get())) vt = cur_ty;
            auto ie = iter_elem(vt, a.line, a.col);
            if (!assignable(cur_ty->sub[0], ie)) {
                if (cur_ty->locked)
                    err(a.line, a.col,
                        std::format("list element type is '{}', got '{}'",
                                    type_name(cur_ty->sub[0]), type_name(ie)));
                else
                    widen_slot(cur_ty->sub[0], cur_ty, ie);
            }
            return;
        }
        auto vt = expr(*a.value);
        // tuple 的原地运算没有运行时入口（降级层会退化成指针算术，实测打出野指针）。
        // 在类型检查就拒绝，别等到 codegen。
        if (cur_ty && cur_ty->kind == TyKind::Tuple &&
            (a.op == Tok::PLUSEQUAL || a.op == Tok::STAREQUAL)) {
            err(a.line, a.col,
                std::format("'{}' does not support in-place operator here; write it out "
                            "explicitly (e.g. res = a + b)",
                            type_name(cur_ty)));
            return;
        }
        // x += y：优先原地 dunder，没有再看普通 __add__ / __radd__。
        if (TypePtr irt = dunder_aug_ret(a.op, cur_ty, vt)) {
            assign(*a.target, irt, a.line, a.col);
            // 注意：这里**不能**同步 expr_types_[target] —— 降级期
            // lower_aug_assign 先用 target 的类型读左操作数（类实例要走
            // __iadd__），同步成返回值类型会让读操作按 int 解引用对象指针
            // （121_decorated_dunder 的 w += 2 实测垃圾值）。
            return;
        }
        Tok bin = aug_to_bin(a.op);
        auto rt = binop_type(bin, cur_ty, vt, a.line, a.col);
        // u64 目标 += u64 大字面量：提升后是 U64，消费字面量（回绕语义不变）。
        consume_u64_literal(rt, a.value.get());
        // 定宽目标的复合赋值按 C 口径把结果**转换回目标类型**（隐式窄化）。
        // x: u8; x += 1 是 u8 的 255+1 → 0，不是静默丢掉高位：
        // 降级层在赋值点按目标类型正常化（回绕是 pa 的确定语义）。
        if (is_fixed_int(cur_ty) && rt && is_int_kind(rt->kind)) rt = cur_ty;
        assign(*a.target, rt, a.line, a.col);
        // 目标表达式在 stmt_aug_assign 开头被求值过，expr_types_ 里缓存的是
        // **加宽前**的类型（int）。assign 已经把变量/元素/字段的槽加宽成
        // 结果类型（int += float → float），但缓存不刷新的话，降级期读目标
        // 还是按 int 读：列表/字典里实际存的是 double，读出来是 1.0 的位
        // 模式 4607182418800017408（打印成 4.6e18），-O2 还会因为 int|double
        // 混算让汇编失败。这里把缓存同步成赋值后的类型。
        expr_types_[a.target.get()] = rt;
        return;
    }

// 免写直调可用标注定返回 ABI 的类型：标量 + 可空 str/ptr。
static bool c_abi_annotation(const TypePtr& t) {
    if (!t) return false;
    switch (t->kind) {
        case TyKind::Float:
        case TyKind::Int:
        case TyKind::Str:
        case TyKind::Bool:
        case TyKind::None: return true;
        case TyKind::Optional:
            return !t->sub.empty() &&
                   (t->sub[0]->kind == TyKind::Str || t->sub[0]->kind == TyKind::Int);
        default: return false;
    }
}

bool TypeChecker::is_c_lazy_call(const Expr* e) const {
    if (!e || e->kind != ExprKind::Call) return false;
    const auto& c = static_cast<const Call&>(*e);
    if (c.func->kind == ExprKind::Name) {
        TypePtr ft = type_of(c.func.get());
        return ft && ft->kind == TyKind::Func && ft->fn && ft->fn->c_lazy;
    }
    if (c.func->kind == ExprKind::Attribute) {
        const auto& at = static_cast<const Attribute&>(*c.func);
        TypePtr rt = type_of(at.value.get());
        return rt && rt->kind == TyKind::DYLIB;
    }
    return false;
}

void TypeChecker::stmt_ann_assign(AnnAssign& a) {
        auto ann = parse_annotation(a.annotation.get(), a.line);
        TypePtr vt;
        if (a.value) {
            vt = expr(*a.value);
            if (ann && is_c_lazy_call(a.value.get()) && c_abi_annotation(ann)) {
                // 标注定返回 ABI：覆盖调用点推断（调用方对 ABI 负责）。
                vt = ann;
                expr_types_[a.value.get()] = ann;
            } else if (ann && (is_fixed_int(ann) || ann->kind == TyKind::Big) &&
                       accept_literal_into(ann, a.value.get())) {
                // 定宽/big 标注 + 字面量：范围内 coerce，超范围已报错。
                vt = ann;
            } else if (ann && coerce_container_literal(ann, a.value.get())) {
                // 定宽元素容器标注 + 字面量容器：逐元素 coerce（同口径）。
                vt = ann;
            } else if (ann && !assignable(ann, vt)) {
                // 定宽/big 标注才给「显式 cast」提示；联合别拼接成
                // 「cast_int | str(x)」这种四不像（0.488 修 Bug A 时发现）。
                const bool cast_hint = !is_union_ty(ann) &&
                                       (is_fixed_int(ann) || ann->kind == TyKind::Big);
                if (cast_hint)
                    err(a.line, a.col,
                        std::format("annotated as '{}' but assigned '{}' (fixed-width needs "
                                    "an explicit cast, e.g. cast_{}(x))",
                                    type_name(ann), type_name(vt), type_name(ann)));
                else
                    err(a.line, a.col,
                        std::format("annotated as '{}' but assigned '{}'", type_name(ann),
                                    type_name(vt)));
            }
        }
        if (auto* n = dynamic_cast<Name*>(a.target.get())) {
            declare(n->id, ann ? ann : t_unknown(), a.line, a.col, true);
        } else if (auto* at = dynamic_cast<Attribute*>(a.target.get());
                   at && is_self_attr(*at)) {
            // `self.x: int = 0`：字段标注写在方法里，等价于类体标注
            // 接收者自己也要过一遍表达式检查：降级层按 expr_types_ 查
            // 接收者的类型来决定走字段存储还是运行期属性表，这里漏掉会让
            // `self.x: int = x` 在代码生成期报「unsupported assignment target」。
            (void)expr(*at->value);
            TypePtr want = (ann && !is_unknown(ann)) ? ann : (vt ? vt : t_unknown());
            if (!add_implicit_field(*at, want)) {
                if (auto* f = cur_class_->field(at->attr);
                    f && !is_unknown(want) && !assignable(f->ty, want))
                    err(a.line, a.col,
                        std::format("field '{}' has type '{}', cannot annotate as '{}'",
                                    at->attr, type_name(f->ty), type_name(want)));
            }
        }
        return;
    }

void TypeChecker::stmt_return(Return& r) {
        TypePtr vt = r.value ? expr(*r.value) : t_none();
        if (!cur_fn_) {
            err(r.line, r.col, "'return' outside function");
            return;
        }
        // 免写直调：声明的返回标注同样可以定返回 ABI。
        if (r.value && cur_fn_->ret_declared && is_c_lazy_call(r.value.get())) {
            TypePtr want = cur_fn_->is_async ? cur_fn_->async_result : cur_fn_->ret;
            if (c_abi_annotation(want)) {
                vt = want;
                expr_types_[r.value.get()] = want;
            }
        }
        if (cur_fn_->ret_declared) {
            TypePtr want = cur_fn_->is_async ? cur_fn_->async_result : cur_fn_->ret;
            // return {}：空字面量的洞按声明返回类型填。
            if (vt && ty_contains_hole(vt, 0)) fill_holes_recursive(vt, want, 0);
            bool lit_ok = r.value && (is_fixed_int(want) || want->kind == TyKind::Big) &&
                          accept_literal_into(want, r.value.get());
            // 定宽元素容器返回标注 + 字面量容器：逐元素 coerce（同口径）。
            if (!lit_ok && r.value && coerce_container_literal(want, r.value.get()))
                lit_ok = true;
            if (lit_ok)
                vt = want;
            else if (!assignable(want, vt))
                err(r.line, r.col,
                    std::format("function '{}' is declared to return '{}', got '{}' "
                                "(fixed-width needs an explicit cast)",
                                cur_fn_->name, type_name(want), type_name(vt)));
        }
        // **始终收集返回值类型**，即使标注了。
        //
        // 标注里可能含「待推断」空洞（裸 `list` / `set` / `dict` 解析成
        // `list[hole]`），需要实际 return 的类型来填。原来只在
        // `!ret_declared` 时收集，`-> list:` 的 hole 就永远填不上，
        // 返回类型退化成 list[unknown] —— 比完全不标注还差。
        if (rets_) rets_->push_back({vt, r.line, r.col});
        return;
    }

void TypeChecker::stmt_if(If& f) {
        // 分支合并：每个分支都从 if 之前的变量状态出发，最后取各分支的
        // 公共上界。直接在原地检查会让 else 分支看到 then 分支的赋值，
        // `if p is None: p = [] else: p = p.split(':')` 就会误判。
        // 只快照「类型」不搬整个 map：外层的收窄持有 Symbol* 指针，
        // 换掉 map 会让这些指针悬空。
        std::unordered_map<std::string, TypePtr> snapshot;
        snapshot.reserve(cur_->vars.size());
        for (auto& [n, sym] : cur_->vars) snapshot.emplace(n, sym.ty);
        auto restore_types = [&]() {
            for (auto& [n, sym] : cur_->vars) {
                auto it = snapshot.find(n);
                if (it != snapshot.end()) sym.ty = it->second;
            }
        };
        // 联合类型以「if 之前的类型」为合并起点（0.529）：分支只做收窄时，
        // join(联合, 收窄成员) 必须回到联合；从空表起步会退化成 join(int, str)=unknown。
        // **只播种联合**——Optional 等不能播种：0.498 的「先合并再恢复窄化」依赖
        // merge 看到的是分支末端的类型，播种 Optional 会把 None 又并回来。
        std::unordered_map<std::string, TypePtr> merged;
        for (auto& [n, ty] : snapshot)
            if (is_union_ty(ty)) merged.emplace(n, ty);
        auto merge_in = [&]() {
            for (auto& [n, sym] : cur_->vars) {
                auto it = merged.find(n);
                if (it == merged.end()) merged.emplace(n, sym.ty);
                else if (!type_eq(it->second, sym.ty)) {
                    TypePtr j = join(it->second, sym.ty);
                    if (type_stats_on_ && !is_unknown(it->second) && !is_unknown(sym.ty) &&
                        is_unknown(j))
                        type_stat("branch_merge", n);
                    it->second = j;
                }
            }
        };
        for (auto& b : f.branches) {
            restore_types();
            expr(*b.cond);
            auto ns = narrow_all(*b.cond, true);
            stmts_block(b.body);
            // 先合并再恢复窄化（0.498）：`if x is None: x = 0` 之后，分支末尾的
            // 类型是 int（赋值 + 窄化），恢复在前会让 merge 又看到 Optional，
            // 合并结果退回归声明类型，下一步 `x + 1` 就误报 may be None。
            merge_in();
            for (auto it = ns.rbegin(); it != ns.rend(); ++it) restore_narrowing(*it);
        }
        restore_types();
        // else 分支等价于「所有条件都为假」，也要收窄：
        // `if x is None: ... else: x.foo()` 里的 x 不是 None
        std::vector<Narrowing> ns;
        // 显式 else 与**隐式空 else** 都要按「条件为假」窄化（0.498）：
        // `if x is None: x = 0` 之后没有 else，但走到下一句时条件必为假，
        // x 就是非 None；漏了这条，分支合并会把 Optional 又并回来。
        if (f.branches.size() == 1) ns = narrow_all(*f.branches[0].cond, false);
        stmts_block(f.orelse);
        merge_in();
        for (auto it = ns.rbegin(); it != ns.rend(); ++it) restore_narrowing(*it);
        // 写回：类型取各分支上界，分支里新出现的名字保留
        // 注意：这里**不能**顺手把模块级 Optional 的 None 去掉 —— 分支合并
        // 的结果同时决定降级层的表示（在模块级 print(d.get(k)) 会因为类型从
        // Optional 变 int 而把 None 打成 0）。模块级的 None 处理只在
        // declare() 里对「已有确定类型又来了一个 Optional」这一种情形做。
        for (auto& [name, ty] : merged) {
            auto it = cur_->vars.find(name);
            if (it == cur_->vars.end())
                cur_->vars.emplace(name, Symbol{ty, f.line, false, false});
            else
                it->second.ty = ty;
        }
        return;
    }

void TypeChecker::stmt_while(While& w) {
        expr(*w.cond);
        ++loop_depth_;
        // 循环体内条件必为真（0.498）：`while cur is not None: cur = cur.next`
        // 里 cur 在体内是非 None。退出后不做反向窄化（循环可能零次执行）。
        auto ns = narrow_all(*w.cond, true);
        stmts_block(w.body);
        for (auto it = ns.rbegin(); it != ns.rend(); ++it) restore_narrowing(*it);
        --loop_depth_;
        stmts_block(w.orelse);
        return;
    }

void TypeChecker::stmt_for(For& f) {
        auto it = expr(*f.iter);
        TypePtr elem;
        if (f.is_async) {
            // 自定义 __aiter__：先静态解析成异步迭代器类型。
            TypePtr iter_ty = it;
            bool via_aiter = false;
            if (it && it->kind == TyKind::Class && it->cls) {
                if (auto* ai = it->cls->method("__aiter__")) {
                    via_aiter = true;
                    iter_ty = t_unknown();
                    if (ai->call_sig && ai->call_sig->ret) iter_ty = ai->call_sig->ret;
                    if (ai->decorated && ai->decorated_ty &&
                        ai->decorated_ty->kind == TyKind::Func &&
                        ai->decorated_ty->fn && ai->decorated_ty->fn->ret)
                        iter_ty = ai->decorated_ty->fn->ret;
                }
            }
            if (is_unknown(it) || (via_aiter && is_unknown(iter_ty))) {
                elem = t_unknown();
            } else if (iter_ty && iter_ty->kind == TyKind::AsyncGenerator) {
                elem = iter_ty->sub.empty() ? t_unknown() : iter_ty->sub[0];
            } else if (via_aiter && iter_ty && iter_ty->kind == TyKind::Class &&
                       iter_ty->cls && iter_ty->cls->method("__anext__")) {
                // __aiter__ 返回自身（或另一个异步迭代器）：每轮
                // await __anext__()，元素类型是协程的结果。
                auto* ne = iter_ty->cls->method("__anext__");
                TypePtr nt = ne->call_sig ? ne->call_sig->ret : nullptr;
                if (ne->decorated && ne->decorated_ty &&
                    ne->decorated_ty->kind == TyKind::Func && ne->decorated_ty->fn)
                    nt = ne->decorated_ty->fn->ret;
                if (nt && nt->kind == TyKind::Coroutine)
                    elem = nt->sub.empty() ? t_unknown() : nt->sub[0];
                else {
                    err(f.iter->line, f.iter->col,
                        std::format("async for requires __anext__ to be async on '{}'",
                                    iter_ty->cls->name));
                    elem = t_unknown();
                }
            } else {
                err(f.iter->line, f.iter->col,
                    std::format("async for requires an async generator or a class "
                                "with __aiter__, got '{}'",
                                type_name(it)));
                elem = t_unknown();
            }
        } else {
            if (it && it->kind == TyKind::AsyncGenerator)
                err(f.iter->line, f.iter->col,
                    "'asyncgen' object is not iterable without 'async for'");
            elem = iter_elem(it, f.line, f.col);
        }
        // 同名循环目标可能被不同元素类型的 for 反复绑定
        // （`for t in [(1, 2, 3)]` 后接 `for t in [(1.5, 4)]`）。符号表里
        // 一个名字只有一个槽类型，declare 会取 join；join 出来的
        // tuple[float, ...] 与循环体内那一轮的真实值表示并不一致：
        // 存储点按槽类型逐元素重建（把 1 变成 1.0），读取点又按当初记录的
        // 元素类型解释（float 位模式当 int 打印 4607182418800017408）。
        // 这里把「本轮元素类型」和「跨轮合并后的槽类型」分开：
        //   * 循环体内按本轮元素类型检查（tc_.type_of 记录的就是它）；
        //   * 与上一轮绑定不同时槽退化成 unknown（存储不做转换，float
        //     标量由既有机制装箱成 CPYFLOAT），循环结束后再合并回去
        //     （循环可能一次都不执行）。
        std::vector<std::pair<std::string, TypePtr>> body_tys;
        collect_target_types(*f.target, elem, body_tys);

        struct ForTarget {
            Scope* sc;
            std::string name;
            TypePtr prev;  // 循环前的类型（不存在为空）
            TypePtr body;  // 循环体内类型
            TypePtr exit;  // 循环结束后的槽类型
        };
        std::vector<ForTarget> binds;
        binds.reserve(body_tys.size());
        for (auto& [name, bty] : body_tys) {
            Scope* sc = assign_target_scope(name);
            auto sit = sc->vars.find(name);
            binds.push_back(ForTarget{sc, name,
                                      sit != sc->vars.end() ? sit->second.ty : nullptr, bty,
                                      nullptr});
        }
        assign(*f.target, elem, f.line, f.col);
        for (auto& b : binds) {
            auto sit = b.sc->vars.find(b.name);
            if (sit == b.sc->vars.end() || sit->second.annotated) continue;
            bool prev_none = b.prev && b.prev->kind == TyKind::None;
            bool prev_concrete = b.prev && b.prev->kind != TyKind::Unknown &&
                                 b.prev->kind != TyKind::None;
            if (prev_concrete && !type_eq(b.prev, b.body)) {
                sit->second.ty = t_unknown();
                sit->second.dynamic = true;
            }
            // 槽仍是 unknown 时（含上面刚退化的）循环体保持元素类型：
            // 元素值的真实表示就是本轮类型，按它读才不会被槽类型带偏。
            if (!prev_none && !is_unknown(sit->second.ty)) b.body = sit->second.ty;
            b.exit = sit->second.ty;
            sit->second.ty = b.body;
        }
        ++loop_depth_;
        stmts_block(f.body);
        --loop_depth_;
        for (auto& b : binds) {
            auto sit = b.sc->vars.find(b.name);
            if (sit == b.sc->vars.end() || sit->second.annotated) continue;
            sit->second.ty = join(b.exit, sit->second.ty);
        }
        stmts_block(f.orelse);
        return;
    }

void TypeChecker::stmt_try(Try& t) {
        // 异常可能在任何一条语句上抛出，所以 except 分支看到的变量
        // 类型是「try 入口」与「try 结束时」的公共上界。
        // 不做这个回滚，`v = int(v)` 之后的 `except: v.replace(...)`
        // 会被当成 int 调 str 方法。
        auto snapshot = cur_->vars;
        stmts_block(t.body);
        for (auto& h : t.handlers) {
            if (h.type) expr(*h.type);
            for (auto& [name, sym] : cur_->vars) {
                if (sym.annotated) continue;
                auto it = snapshot.find(name);
                if (it == snapshot.end()) continue;
                if (type_eq(sym.ty, it->second.ty)) continue;
                sym.ty = join(sym.ty, it->second.ty);
            }
            if (!h.name.empty()) {
                // as e 绑定异常值对象。富异常类（有字段/方法）绑定成真实例，
                // 字段/方法走普通类访问；pass-only 或内置异常仍是 Exception 值。
                //
                // except (A, B) as e：逐个解析分支，**全部解析成同一个类型**时
                // 用它（`except (MyErr, MyErr)` 这类实际是同一个类的写法），
                // 否则退到 Exception 基类 —— 多类型的方法面是交集，静态上不
                // 硬造 union（见 docs/frontend-gaps.md）。
                auto resolve_exc = [&](Expr* e) -> TypePtr {
                    if (!e || e->kind != ExprKind::Name) return nullptr;
                    auto& hn = static_cast<Name&>(*e);
                    if (is_exception_name(hn.id)) return mk_exception(hn.id);
                    if (auto* sym = lookup(hn.id);
                        sym && sym->ty && sym->ty->kind == TyKind::Class && sym->ty->cls &&
                        sym->ty->cls->is_exception_class)
                        return sym->ty->cls->is_rich_exception ? mk_class(sym->ty->cls)
                                                               : mk_exception(sym->ty->cls->name);
                    return nullptr;
                };
                TypePtr et = mk_exception("Exception");
                if (h.type && h.type->kind == ExprKind::Tuple) {
                    TypePtr acc = nullptr;
                    bool same = true;
                    for (auto& el : static_cast<TupleExpr&>(*h.type).elts) {
                        TypePtr one = resolve_exc(el.get());
                        if (!one) {
                            same = false;
                            break;
                        }
                        if (!acc)
                            acc = one;
                        else if (!type_eq(acc, one))
                            same = false;
                    }
                    if (acc && same) et = acc;
                } else if (auto one = resolve_exc(h.type.get())) {
                    et = one;
                }
                auto sc = std::make_unique<Scope>();
                sc->parent = cur_;
                cur_ = sc.get();
                pool_.push_back(std::move(sc));
                cur_->vars[h.name] = Symbol{et, t.line, false, false};
                stmts_block(h.body);
                // Python 是**函数级作用域**：handler 体里绑定的名字（除 as e 外）
                // 属于函数作用域，必须回写到父作用域。只建临时作用域放 e 会让
                // `x = str(e)` 在子作用域里造一个同名新符号，外层 x 保持旧类型；
                // 降级按名字写同一个槽，print 却按旧类型解释新值（实测：int 槽
                // 存 str 指针，打印出一串数字而不是异常消息）。
                for (auto& [nm, sym] : cur_->vars) {
                    if (nm == h.name) continue;
                    auto pit = cur_->parent->vars.find(nm);
                    if (pit == cur_->parent->vars.end())
                        cur_->parent->vars[nm] = sym;
                    else
                        pit->second = sym;
                }
                cur_ = cur_->parent;
            } else {
                stmts_block(h.body);
            }
        }
        stmts_block(t.orelse);
        stmts_block(t.finalbody);
        return;
    }

void TypeChecker::stmt_with(With& w) {
        for (auto& item : w.items) {
            auto t = expr(*item.context);
            // `with Ctx() as c` 的 c 是 __enter__ 的返回值；能查到
            // 方法签名就用它，查不到退回 unknown（with 本身不检查协议）。
            TypePtr vt = t_unknown();
            TypePtr ct = t;
            if (ct && ct->kind == TyKind::Optional && !ct->sub.empty()) ct = ct->sub[0];
            if (ct && ct->kind == TyKind::Class && ct->cls) {
                const char* enter = w.is_async ? "__aenter__" : "__enter__";
                if (auto* m = ct->cls->method(enter); m && m->call_sig && m->call_sig->ret) {
                    vt = m->call_sig->ret;
                    // 装饰后的方法调用的是类属性里的包装函数，
                    // 绑定值按包装函数的返回类型算。
                    if (m->decorated && m->decorated_ty &&
                        m->decorated_ty->kind == TyKind::Func && m->decorated_ty->fn &&
                        m->decorated_ty->fn->ret)
                        vt = m->decorated_ty->fn->ret;
                }
                if (w.is_async) {
                    // 异步上下文管理器：__aenter__/__aexit__ 必须
                    // 是 async def，绑定的是 await 之后的结果。
                    auto* ex = ct->cls->method("__aexit__");
                    if (!vt || vt->kind != TyKind::Coroutine)
                        err(item.context->line, item.context->col,
                            std::format("'{}' does not define async __aenter__",
                                        ct->cls->name));
                    else
                        vt = vt->sub.empty() ? t_unknown() : vt->sub[0];
                    if (!ex || !ex->call_sig || !ex->call_sig->ret ||
                        ex->call_sig->ret->kind != TyKind::Coroutine)
                        err(item.context->line, item.context->col,
                            std::format("'{}' does not define async __aexit__",
                                        ct->cls->name));
                }
            } else if (w.is_async && ct && ct->kind == TyKind::AsyncLock) {
                // 内置 Lock 的 __aenter__ 就是 await acquire()。
                vt = t_bool();
            } else if (w.is_async && ct && ct->kind == TyKind::AsyncEvent) {
                vt = t_bool();
            } else if (w.is_async && ct && ct->kind == TyKind::AsyncSemaphore) {
                vt = t_bool();
            } else if (w.is_async && ct && ct->kind == TyKind::AsyncTimeout) {
                vt = t_none();
            } else if (w.is_async && ct && ct->kind == TyKind::AsyncTaskGroup) {
                // __aenter__ 返回 TaskGroup 自身，as tg 绑定的是管理器。
                vt = ct;
            } else if (ct && ct->kind == TyKind::File) {
                // with open(...) as f：f 就是文件对象本身。
                vt = ct;
            } else if (w.is_async) {
                // 目标类型未知/跨模块（timeouts.timeout 等）或类实例的
                // __aenter__ 在别处定义：conservative 放过。
                if (ct && ct->kind != TyKind::Unknown && ct->kind == TyKind::Class &&
                    ct->cls && !ct->cls->is_open() && !imported_base_in_mro(ct->cls))
                    err(item.context->line, item.context->col,
                        "async with requires a class instance with async "
                        "__aenter__/__aexit__");
            }
            if (item.optional_vars)
                assign(*item.optional_vars, vt, w.line, w.col);
        }
        stmts_block(w.body);
        return;
    }

void TypeChecker::stmt_raise(Raise& r) {
        if (r.exc) expr(*r.exc);
        if (r.cause) {
            auto ct = expr(*r.cause);
            if (!is_unknown(ct) && ct->kind != TyKind::None &&
                ct->kind != TyKind::Exception)
                err(r.cause->line, r.cause->col,
                    "raise from requires an exception or None");
        }
        return;
    }

void TypeChecker::stmt_assert(Assert& a) {
        expr(*a.test);
        if (a.msg) expr(*a.msg);
        // `assert x is not None` 之后 x 一定非 None（0.498）：只在这里施加窄化，
        // 不登记恢复点 —— 它的作用域就是余下的语句；外层 if 分支的
        // restore_types 快照会自然地把它限制在该分支内。
        (void)narrow_on(*a.test, true);
        return;
    }

void TypeChecker::stmt_import(Import& im) {
        for (auto& al : im.names) {
            std::string bind = al.asname.empty() ? al.name : al.asname;
            if (auto pos = bind.find('.'); pos != std::string::npos) bind = bind.substr(0, pos);
            if (!module_lookup_) {
                // 单文件 check：import 没建模，保持前端宽松。
                declare(bind, t_unknown(), im.line);
                unsupported.push_back("import");
                continue;
            }
            if (al.name.find('.') != std::string::npos && al.asname.empty()) {
                if (lenient_imports_) {
                    declare(bind, t_unknown(), im.line);
                    unsupported.push_back("import");
                    continue;
                }
                err(im.line, im.col,
                    "dotted import needs 'as name' (or use from ... import)");
                continue;
            }
            if (al.name == "python" || al.name.starts_with("python.")) {
                err(im.line, im.col,
                    "use 'from python import <module>' instead of 'import python'");
                continue;
            }
            ModuleInfoPtr info = module_lookup_(al.name);
            if (info) {
                cur_->modules[bind] = std::move(info);
            } else {
                declare(bind, t_unknown(), im.line);
                unsupported.push_back("import");
            }
        }
        return;
    }

void TypeChecker::stmt_cimport(CImport& ci) {
        std::string dir = std::filesystem::path(file_).parent_path().string();
        for (auto& al : ci.names) {
            std::string bind = al.asname;
            if (bind.empty()) {
                auto dot = al.name.rfind('.');
                bind = dot == std::string::npos ? al.name : al.name.substr(dot + 1);
            }
            if (lookup(bind)) {
                err(ci.line, ci.col,
                    std::format("name '{}' is already defined in this module", bind));
                continue;
            }
            if (cmodules_.count(bind)) {
                err(ci.line, ci.col,
                    std::format("cimport name '{}' is already defined", bind));
                continue;
            }
            std::string load_err;
            auto decl = load_cpya(al.name, dir, cinclude_dirs_, load_err);
            if (!decl) {
                err(ci.line, ci.col, load_err);
                continue;
            }
            auto shared = std::make_shared<CImportDecl>(std::move(*decl));
            std::unordered_map<std::string, StructInfo*> cmap;
            for (auto& cs : shared->structs) {
                auto it = structs_.find(cs.name);
                if (it != structs_.end()) {
                    // 同名 struct 必须来自另一个 cimport 且布局一致；
                    // 与 .pya struct 冲突时不能悄悄用错布局。
                    if (!it->second->c_struct || it->second->size != cs.size ||
                        it->second->align != cs.align) {
                        err(ci.line, ci.col,
                            std::format("struct '{}' is already defined", cs.name));
                    }
                    cmap[cs.name] = it->second;
                    continue;
                }
                StructInfo* st = build_cstruct(cs);
                structs_[cs.name] = st;
                // 允许在 PyAdamas 里按字段构造 C 结构体（DivT(7, 3)）；
                // 已有同名符号时不覆盖。
                if (!lookup(cs.name))
                    module_->vars[cs.name] = Symbol{mk_struct(st), ci.line, true, false};
                cmap[cs.name] = st;
            }
            auto cty = [&](CKind k, const std::string& sname) -> TypePtr {
                if (!sname.empty()) {
                    auto it = cmap.find(sname);
                    if (it != cmap.end()) {
                        if (k == CKind::Ptr) return mk_optional(mk_struct_ptr(it->second));
                        if (k == CKind::Struct) return mk_struct(it->second);
                    }
                }
                return ckind_type(k);
            };
            for (auto& ex : shared->externs) {
                auto fi = std::make_shared<FuncInfo>();
                fi->name = ex.py_name;
                fi->is_extern = true;
                fi->ret_declared = true;
                fi->ret_known = true;
                fi->ret = cty(ex.ret, ex.ret_struct);
                if (ex.ret == CKind::Ptr && ex.ret_elem != CKind::None && fi->ret)
                    fi->ret->celem = ex.ret_elem;
                if (ex.varargs) {
                    fi->varargs = true;
                    fi->vararg_ty = t_unknown();
                }
                for (auto& p : ex.params) {
                    Param pm;
                    pm.name = p.name;
                    pm.ty = cty(p.kind, p.ptr_struct);
                    if (p.kind == CKind::Ptr && p.ptr_elem != CKind::None && pm.ty)
                        pm.ty->celem = p.ptr_elem;
                    if (p.kind == CKind::Ptr) {
                        pm.cptr = true;
                        pm.cptr_elem = p.ptr_elem;
                        // 所有结构体指针都不接受 list/bytes 缓冲区。
                        pm.cptr_opaque = p.opaque || !p.ptr_struct.empty();
                    }
                    fi->pos.push_back(std::move(pm));
                }
                extern_by_fn_[fi.get()] = &ex;
                extern_fns_[&ex] = std::move(fi);
            }
            cmodules_[bind] = std::move(shared);
        }
        return;
    }

void TypeChecker::stmt_import_from(ImportFrom& im) {
        // from c import X：免写直调。X 绑定成「惰性 C 函数」——没有 .cpya
        // 声明，参数编组由调用点实参类型推断，返回类型按推断/标注，符号
        // 在运行期经 dlsym(RTLD_DEFAULT) 解析。调用方对 ABI 负责。
        if (im.level == 0 && im.module == "c") {
            for (auto& al : im.names) {
                if (al.name == "*") {
                    err(im.line, im.col,
                        "'from c import *' is not supported; import C symbols explicitly");
                    continue;
                }
                std::string bind = al.asname.empty() ? al.name : al.asname;
                // 名字可能已被 hoist_module_bindings 预绑定成 unknown 占位，
                // 与普通 from-import 一样直接 declare 覆盖。
                auto fi = std::make_shared<FuncInfo>();
                fi->name = bind;
                fi->c_lazy = true;
                fi->c_symbol = al.name;
                fi->is_extern = true;
                fi->varargs = true;  // 参数个数/种类由调用点决定
                fi->vararg_ty = t_unknown();
                fi->ret_known = false;
                auto ft = new_ty(TyKind::Func);
                ft->fn = fi;
                declare(bind, ft, im.line);
            }
            return;
        }
        if (!module_lookup_) {
            for (auto& al : im.names) {
                std::string bind = al.asname.empty() ? al.name : al.asname;
                if (bind == "*") {
                    star_import_ = true;
                    continue;
                }
                declare(bind, t_unknown(), im.line);
            }
            unsupported.push_back("import");
            return;
        }
        if (im.level != 0 || im.module.empty()) {
            // 相对导入在 check 模式下没有包上下文；lenient 时声明成
            // unknown 放过，交给 build 阶段的完整 import 图。
            if (lenient_imports_) {
                for (auto& al : im.names) {
                    std::string bind = al.asname.empty() ? al.name : al.asname;
                    if (bind == "*") { star_import_ = true; continue; }
                    declare(bind, t_unknown(), im.line);
                }
                unsupported.push_back("relative import");
                return;
            }
            err(im.line, im.col, "relative imports are not supported yet");
            return;
        }
        if (im.names.size() == 1 && im.names[0].name == "*") {
            ModuleInfoPtr info = module_lookup_(im.module);
            if (info && info->cpy_proxy) {
                err(im.line, im.col, "from python import * is not supported");
                return;
            }
            if (!info) {
                star_import_ = true;
                unsupported.push_back("import");
                return;
            }
            for (auto& [n, ty] : info->globals)
                if (!n.empty() && n[0] != '_') declare(n, ty, im.line);
            for (auto& [n, ic] : info->classes) {
                if (n.empty() || n[0] == '_') continue;
                declare(n, mk_class(ic.cls.get()), im.line);
                class_obj_names_.insert(n);
            }
            return;
        }
        ModuleInfoPtr info = module_lookup_(im.module);
        if (info && info->cpy_proxy) {
            // from python import <module> [as alias]：绑定 CPython 模块代理。
            // 代理没有签名表，属性/调用按 unknown 放行（expr / call_forms）。
            for (auto& al : im.names) {
                std::string bind = al.asname.empty() ? al.name : al.asname;
                ModuleInfoPtr child = module_lookup_(im.module + "." + al.name);
                if (!child) {
                    err(im.line, im.col,
                        std::format("cannot resolve CPython module '{}'", al.name));
                    continue;
                }
                cur_->modules[bind] = std::move(child);
            }
            return;
        }
        for (auto& al : im.names) {
            std::string bind = al.asname.empty() ? al.name : al.asname;
            if (!info) {
                declare(bind, t_unknown(), im.line);
                unsupported.push_back("import");
                continue;
            }
            if (auto ci = info->classes.find(al.name); ci != info->classes.end()) {
                declare(bind, mk_class(ci->second.cls.get()), im.line);
                class_obj_names_.insert(bind);
                continue;
            }
            auto it = info->globals.find(al.name);
            if (it == info->globals.end()) {
                if (auto child = module_lookup_(im.module + "." + al.name)) {
                    cur_->modules[bind] = std::move(child);
                    continue;
                }
                if (!reject_message(im.module, al.name).empty()) {
                    err_member(im.module, al.name, im.line, im.col);
                } else if (lenient_imports_) {
                    unsupported.push_back("import attribute");
                } else {
                    err_member(im.module, al.name, im.line, im.col);
                }
                declare(bind, t_unknown(), im.line);
                continue;
            }
            if (info->builtin && info->name == "asyncio")
                native_aliases_[bind] = NativeAlias{info->name, al.name, it->second};
            declare(bind, it->second, im.line);
        }
        return;
    }

void TypeChecker::stmt_delete(Delete& a) {
        for (auto& t : a.targets) {
            expr(*t);
            // 0.537（保守版）：只记名字目标；同一 statements 序列内后续读到时报错。
            if (t && t->kind == ExprKind::Name) deleted_names_.insert(static_cast<Name&>(*t).id);
        }
        return;
    }

}  // namespace pyadamas
