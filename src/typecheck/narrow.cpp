// typecheck/stmt.cpp —— 语句类型检查：控制流、try/with、赋值与解包
#include "driver/build.h"
#include "typecheck/typecheck.h"

#include <filesystem>
#include <set>

#include "ir/module.h"


namespace pyadamas {

// 流窄化（if/isinstance/None 判定）：条件到类型的推导

bool TypeChecker::maybe_none(const Expr* e, const TypePtr& t, int line, int col) {
    if (!t || t->kind != TyKind::Optional) return false;
    if (auto* n = e ? dynamic_cast<const Name*>(e) : nullptr)
        err(line, col, std::format("'{}' may be None here; check it first "
                                   "(if {} is not None:) or use assert",
                                   n->id, n->id));
    else
        err(line, col, "this value may be None here; check it first "
                        "(if x is not None:) or use assert");
    return true;
}

// 从条件表达式里取「可窄化的名字」：裸名字，或 walrus 的被赋值目标
// （`if (n := d.get(k)) is not None:` 之后要窄化的是 n）。
Name* TypeChecker::narrow_name_of(Expr* e) {
    if (!e) return nullptr;
    if (auto* n = dynamic_cast<Name*>(e)) return n;
    if (auto* w = dynamic_cast<NamedExpr*>(e))
        return w->target ? dynamic_cast<Name*>(w->target.get()) : nullptr;
    return nullptr;
}

std::vector<TypeChecker::Narrowing> TypeChecker::narrow_all(Expr& cond, bool truthy) {
    std::vector<Narrowing> out;
    // `A and B` 为真 ⇒ A、B 都为真（短路窄化要叠加）；`A or B` 为假 ⇒ 两者都假。
    if (cond.kind == ExprKind::BoolOp) {
        auto& b = static_cast<BoolOp&>(cond);
        const bool conj = b.op == Tok::KW_AND;
        if (conj == truthy) {
            for (auto& v : b.values) {
                auto sub = narrow_all(*v, truthy);
                for (auto& n : sub) out.push_back(n);
            }
            return out;
        }
        return out;  // 混合极性（and 为假 / or 为真）不窄化，保守
    }
    auto one = narrow_on(cond, truthy);
    if (one.sym) out.push_back(one);
    return out;
}

// 内置类型的 isinstance 窄化（`isinstance(o, int/str/float/list/...)`）。
//
// 以前只认用户类（narrow_isinstance 需要 ClassInfo），内置类型的 isinstance
// **完全不窄化**：`if isinstance(o, int): return` 之后的 o 仍是 `int | str`。
// 0.529 的 P0-D 于是让降级层按「唯一命中方法」的启发式挑成员来兜底
// （tests/run/isinstance_neg_branch.py 就是这类形状），那条启发式对真·多类
// 联合是静默错值（0.679 审计 r13/r15）。补上按 TyKind 的收窄之后，降级层
// 可以只对「非 None 成员恰好一个」的形态静态选型。
namespace {
bool builtin_kind_matches(const TypePtr& m, const TypePtr& want) {
    if (!m || !want) return false;
    switch (want->kind) {
        // CPython：bool 是 int 的子类；定宽整数也是 int。
        case TyKind::Int:
            return m->kind == TyKind::Int || m->kind == TyKind::Bool || is_fixed_int(m);
        case TyKind::Bool: return m->kind == TyKind::Bool;
        case TyKind::Float: return m->kind == TyKind::Float;
        case TyKind::Str: return m->kind == TyKind::Str;
        case TyKind::Bytes: return m->kind == TyKind::Bytes;
        case TyKind::Complex: return m->kind == TyKind::Complex;
        case TyKind::List: return m->kind == TyKind::List;
        case TyKind::Dict: return m->kind == TyKind::Dict;
        case TyKind::Tuple: return m->kind == TyKind::Tuple;
        case TyKind::Set: return m->kind == TyKind::Set;
        default: return false;
    }
}
}  // namespace

// 真值分支留下 kind 兼容的成员（并去掉 None）；假值分支去掉它们。
// 判不准（没有命中 / 去掉就空）返回 nullptr，保持原类型。
static TypePtr narrow_isinstance_builtin(const TypePtr& t, const TypePtr& want, bool truthy) {
    if (!t || !want) return nullptr;
    std::vector<TypePtr> members;
    bool optional = false;
    if (t->kind == TyKind::Optional) {
        optional = true;
        if (t->sub.empty() || !t->sub[0]) return nullptr;
        union_flatten(t->sub[0], members);
    } else if (is_union_ty(t)) {
        union_flatten(t, members);
    } else {
        members.push_back(t);
    }
    if (members.empty()) return nullptr;
    std::vector<TypePtr> keep;
    int hits = 0;
    for (auto& m : members) {
        const bool hit = builtin_kind_matches(m, want);
        if (hit) ++hits;
        if (truthy ? hit : !hit) keep.push_back(m);
    }
    if (hits == 0) return nullptr;              // 联合里没有这种内置类型：不动
    if (!truthy && keep.empty()) return nullptr;  // 去掉就空：不动（保守）
    TypePtr acc = nullptr;
    for (auto& m : keep) acc = acc ? mk_union(acc, m) : m;
    if (!acc) return nullptr;
    // 真值分支一定非 None；假值分支在 Optional 上保留 Optional 包装。
    if (truthy || !optional) return acc;
    return mk_optional(acc);
}

// isinstance 窄化：真值分支取 cls（联合里必须至少有一个与之兼容的类成员），
// 假值分支把「cls 或其子类」的成员去掉。判不准返回 nullptr（保持原类型）。
TypePtr TypeChecker::narrow_isinstance(const TypePtr& t, const ClassInfo* cls, bool truthy) {
    if (!t || !cls) return nullptr;
    std::vector<TypePtr> members;
    bool optional = false;
    if (t->kind == TyKind::Optional) {
        optional = true;
        if (t->sub.empty() || !t->sub[0]) return nullptr;
        union_flatten(t->sub[0], members);
    } else if (is_union_ty(t)) {
        union_flatten(t, members);
    } else {
        members.push_back(t);
    }
    if (members.empty()) return nullptr;
    auto exactly = [&](const TypePtr& m) {
        if (!m || m->kind != TyKind::Class || !m->cls) return false;
        return m->cls == cls || m->cls->is_subclass_of(cls);
    };
    auto compat = [&](const TypePtr& m) {
        if (!m || m->kind != TyKind::Class || !m->cls) return false;
        return m->cls == cls || m->cls->is_subclass_of(cls) || cls->is_subclass_of(m->cls);
    };
    if (truthy) {
        bool any = false;
        for (auto& m : members)
            if (compat(m)) any = true;
        if (!any) return nullptr;  // 联合里没有这个类：判不准，不动
        return mk_class(const_cast<ClassInfo*>(cls));
    }
    std::vector<TypePtr> keep;
    bool removed = false;
    for (auto& m : members) {
        if (exactly(m)) {
            removed = true;
            continue;
        }
        keep.push_back(m);
    }
    if (!removed || keep.empty()) return nullptr;  // 去掉就空了：判不准
    TypePtr acc = nullptr;
    for (auto& m : keep) acc = acc ? mk_union(acc, m) : m;
    if (!acc) return nullptr;
    return optional ? mk_optional(acc) : acc;
}

TypeChecker::Narrowing TypeChecker::narrow_on(Expr& cond, bool truthy) {
        auto non_none = [](const TypePtr& t) -> TypePtr {
            if (!t) return t;
            if (t->kind == TyKind::Optional) {
                // 0.563：`ptr[T]` 就是 Optional[int]+celem，收窄不能拆成纯 int（否则 p[i] 报
                // "int 不可下标"）；形状原样保留，运行期 guard_not_none 照旧检查。
                if (t->celem != CKind::None || !t->cdims.empty()) return t;
                return t->sub.empty() ? t_unknown() : t->sub[0];
            }
            if (t->kind == TyKind::None) return t_unknown();  // 分支实际不可达
            return t;
        };
        auto is_none_const = [](Expr* e) {
            auto* c = dynamic_cast<Constant*>(e);
            return c && c->ck == Constant::CK::None;
        };

        if (cond.kind == ExprKind::UnaryOp) {
            auto& u = static_cast<UnaryOp&>(cond);
            if (u.op == Tok::KW_NOT) return narrow_on(*u.operand, !truthy);
        }
        if (cond.kind == ExprKind::Compare) {
            auto& c = static_cast<Compare&>(cond);
            if (c.ops.size() == 1) {
                // `(n := expr) is not None`：walrus 也应该被窄化 —— 参与判断的
                // 是**被赋名的那个变量**（graphlib 的 nodeinfo 就是这种写法）。
                Name* name = narrow_name_of(c.left.get());
                bool none_right = is_none_const(c.comparators[0].get());
                if (!name && is_none_const(c.left.get()))
                    name = narrow_name_of(c.comparators[0].get());
                if (!name) {
                    // 属性收窄：`if self.x is not None:` → 之后 `self.x.foo()`。
                    // 只认单层「名字.属性」，键 action 存 attr_refine_。
                    Expr* oe = none_right ? c.left.get() : c.comparators[0].get();
                    auto* at = dynamic_cast<Attribute*>(oe);
                    auto* bn = at ? dynamic_cast<Name*>(at->value.get()) : nullptr;
                    if (at && bn) {
                        bool is_not2 = c.ops[0] == CmpOp::IsNot;
                        if (is_not2 != truthy) return {};
                        TypePtr cur = expr(*at);
                        Narrowing n;
                        n.akey = bn->id + "." + at->attr;
                        auto it2 = attr_refine_.find(n.akey);
                        n.had_old = it2 != attr_refine_.end();
                        n.old = n.had_old ? it2->second : nullptr;
                        n.now = non_none(cur);
                        attr_refine_[n.akey] = n.now;
                        return n;
                    }
                }
                if (name && (none_right || is_none_const(c.left.get()))) {
                    // 只做 "is not None" 方向的收窄。
                    // "is None" 分支里若把变量改成 None，`if x is None: x = {}`
                    // 这种合法写法反而会报错，收窄必须尊重原声明。
                    bool is_not = c.ops[0] == CmpOp::IsNot;
                    if (is_not != truthy) return {};
                    Symbol* sym = lookup(name->id);
                    if (!sym) return {};
                    Narrowing n{};
                    n.sym = sym;
                    n.old = sym->ty;
                    n.now = nullptr;
                    sym->ty = non_none(sym->ty);
                    n.now = sym->ty;
                    return n;
                }
            }
        }
        // isinstance(x, T)：真值分支把联合收窄成 T，假值分支把 T 去掉。
        // T 可以是用户类（narrow_isinstance）或内置类型（按 TyKind 收窄，
        // `isinstance(o, int)` 之后 o: int|str → int / str）。元组形式与属性
        // 目标仍不窄化（保守：判不准就不动，避免误杀）。
        if (cond.kind == ExprKind::Call) {
            auto& call = static_cast<Call&>(cond);
            auto* cfn = dynamic_cast<Name*>(call.func.get());
            if (cfn && cfn->id == "isinstance" && !lookup("isinstance") &&
                call.keywords.empty() && call.args.size() == 2) {
                Name* tgt = narrow_name_of(call.args[0].get());
                TypePtr ct = expr(*call.args[1]);
                const ClassInfo* cls = nullptr;
                if (ct && ct->kind == TyKind::ClassObj && !ct->sub.empty() && ct->sub[0] &&
                    ct->sub[0]->kind == TyKind::Class)
                    cls = ct->sub[0]->cls;
                // 内置类型名（int/str/float/...）是 TypeObj：按 kind 窄化联合。
                TypePtr want_builtin;
                if (!cls && ct && ct->kind == TyKind::TypeObj && !ct->sub.empty())
                    want_builtin = ct->sub[0];
                Symbol* sym = tgt ? lookup(tgt->id) : nullptr;
                if (sym && sym->ty && (cls || want_builtin)) {
                    TypePtr narrowed = cls ? narrow_isinstance(sym->ty, cls, truthy)
                                           : narrow_isinstance_builtin(sym->ty, want_builtin,
                                                                       truthy);
                    if (narrowed && !type_eq(narrowed, sym->ty)) {
                        Narrowing n{};
                        n.sym = sym;
                        n.old = sym->ty;
                        sym->ty = narrowed;
                        n.now = narrowed;
                        return n;
                    }
                }
            }
        }
        if ((cond.kind == ExprKind::Name || cond.kind == ExprKind::NamedExpr) && truthy) {
            Name* nn = narrow_name_of(&cond);
            if (!nn) return {};
            auto& n = *nn;
            if (Symbol* sym = lookup(n.id)) {
                Narrowing r{};
                r.sym = sym;
                r.old = sym->ty;
                r.now = nullptr;
                sym->ty = non_none(sym->ty);
                r.now = sym->ty;
                return r;
            }
        }
        return {};
    }

    void TypeChecker::restore_narrowing(const Narrowing& n) {
        if (!n.akey.empty()) {
            if (n.had_old) attr_refine_[n.akey] = n.old;
            else attr_refine_.erase(n.akey);
            return;
        }
        // 块内可能已经重新赋值（类型变了），这时不该把类型恢复成收窄前的
        if (!n.sym || n.sym->ty != n.now) return;
        n.sym->ty = n.old;
    }


}  // namespace pyadamas
