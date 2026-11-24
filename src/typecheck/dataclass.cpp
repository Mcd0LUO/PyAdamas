// typecheck/dataclass.cpp —— P4 块4（lang_gaps，0.346）：@dataclass 编译期展开
//
// 设计：docs/lang-gaps-landing.md §8 块4。类体字段收集完后（classdef_impl
// 里调用），按字段注解合成 __init__/__repr__/__eq__ 的 FunctionDef AST 注入
// ClassDef.body，之后的方法签名登记/槽分配/方法体检查/降级/跨模块接口编码
// 全部复用既有机制——不是运行期反射。
//
// v1 范围（保守）：单类（无基类）、字面量默认值、标量/str/bytes/容器字段。
// 继承 dataclass / field() / frozen / order / kw_only / 非字面量默认 /
// 可变字面量默认 / 默认参序违规 → 编译期明确报错（门控开才报；门控关时
// 这些程序本来就编不过或构造不工作，行为逐字不变）。
//
// CPython 口径：用户已定义的 __init__/__repr__/__eq__ 不覆盖（逐方法判）；
// 定义 __eq__ 无 __hash__ → unhashable（lower_builtin_reflect 既有规则自动
// 生效）；__eq__ 只与同类实例比（isinstance 守卫），异类返回 False。
#include "typecheck/typecheck.h"

namespace pyadamas {

namespace {

// 注解 AST 深克隆（只覆盖 v1 允许的标注形态；其它形态返回 nullptr →
// 调用方按「不支持的标注」报错）。
ExprPtr clone_annotation(const Expr* e) {
    if (!e) return nullptr;
    switch (e->kind) {
        case ExprKind::Name: {
            auto n = std::make_unique<Name>(static_cast<const Name*>(e)->id);
            n->line = e->line; n->col = e->col;
            return n;
        }
        case ExprKind::Attribute: {
            auto& a = *static_cast<const Attribute*>(e);
            auto v = clone_annotation(a.value.get());
            if (!v) return nullptr;
            auto n = std::make_unique<Attribute>();
            n->value = std::move(v);
            n->attr = a.attr;
            n->line = e->line; n->col = e->col;
            return n;
        }
        case ExprKind::Subscript: {
            auto& s = *static_cast<const Subscript*>(e);
            auto v = clone_annotation(s.value.get());
            auto sl = clone_annotation(s.slice.get());
            if (!v || !sl) return nullptr;
            auto n = std::make_unique<Subscript>();
            n->value = std::move(v);
            n->slice = std::move(sl);
            n->line = e->line; n->col = e->col;
            return n;
        }
        case ExprKind::Constant: {
            auto& c = *static_cast<const Constant*>(e);
            auto n = std::make_unique<Constant>();
            n->ck = c.ck; n->b = c.b; n->i = c.i; n->f = c.f; n->im = c.im; n->s = c.s;
            n->line = e->line; n->col = e->col;
            return n;
        }
        case ExprKind::Tuple: {
            auto& t = *static_cast<const TupleExpr*>(e);
            auto n = std::make_unique<TupleExpr>();
            for (auto& el : t.elts) {
                auto c = clone_annotation(el.get());
                if (!c) return nullptr;
                n->elts.push_back(std::move(c));
            }
            n->line = e->line; n->col = e->col;
            return n;
        }
        default:
            return nullptr;
    }
}

ExprPtr mk_name(const std::string& id, int line, int col) {
    auto n = std::make_unique<Name>(id);
    n->line = line; n->col = col;
    return n;
}

ExprPtr mk_attr(ExprPtr base, const std::string& attr, int line, int col) {
    auto n = std::make_unique<Attribute>();
    n->value = std::move(base);
    n->attr = attr;
    n->line = line; n->col = col;
    return n;
}

ExprPtr mk_str(const std::string& s, int line, int col) {
    auto n = std::make_unique<Constant>();
    n->ck = Constant::CK::Str;
    n->s = s;
    n->line = line; n->col = col;
    return n;
}

ExprPtr mk_add(ExprPtr l, ExprPtr r, int line, int col) {
    auto n = std::make_unique<BinOp>(Tok::PLUS, std::move(l), std::move(r));
    n->line = line; n->col = col;
    return n;
}

// 字段类型是否在 v1 允许集合里（容器递归查元素）。
bool dc_field_type_ok(const TypePtr& t, int depth = 0) {
    if (!t || depth > 8) return false;
    switch (t->kind) {
        case TyKind::Int: case TyKind::Float: case TyKind::Bool:
        case TyKind::Str: case TyKind::Bytes:
            return true;
        case TyKind::List: case TyKind::Set:
            return !t->sub.empty() && dc_field_type_ok(t->sub[0], depth + 1);
        case TyKind::Dict:
            return t->sub.size() >= 2 && dc_field_type_ok(t->sub[0], depth + 1) &&
                   dc_field_type_ok(t->sub[1], depth + 1);
        case TyKind::Tuple:
            if (t->variadic)
                return !t->sub.empty() && dc_field_type_ok(t->sub[0], depth + 1);
            for (auto& s : t->sub)
                if (!dc_field_type_ok(s, depth + 1)) return false;
            return !t->sub.empty();
        default:
            return false;
    }
}

bool body_has_method(const ClassDef& c, const std::string& name) {
    for (auto& s : c.body)
        if (s->kind == StmtKind::FunctionDef &&
            static_cast<FunctionDef&>(*s).name == name)
            return true;
    return false;
}

}  // namespace

void TypeChecker::expand_dataclass(ClassDef& c, ClassInfoPtr& cls) {
    const int line = c.line, col = c.col;
    auto reject = [&](const std::string& msg) {
        err(line, col, std::format("dataclass '{}': {} (lang-gaps v1)", c.name, msg));
    };

    // ── 装饰器形态：单个 @dataclass / @dataclasses.dataclass / @dataclass(...) ──
    if (c.decorators.size() != 1) {
        reject("multiple decorators are not supported");
        return;
    }
    const Expr* dec = c.decorators[0].get();
    const Expr* head = dec;
    const Call* dcall = nullptr;
    if (dec->kind == ExprKind::Call) {
        dcall = static_cast<const Call*>(dec);
        head = dcall->func.get();
    }
    bool name_ok = false;
    if (head->kind == ExprKind::Name)
        name_ok = static_cast<const Name*>(head)->id == "dataclass";
    else if (head->kind == ExprKind::Attribute) {
        auto& a = *static_cast<const Attribute*>(head);
        name_ok = a.attr == "dataclass" && a.value->kind == ExprKind::Name &&
                  static_cast<const Name*>(a.value.get())->id == "dataclasses";
    }
    if (!name_ok) {
        reject("unrecognized dataclass decorator form");
        return;
    }
    if (dcall) {
        if (!dcall->args.empty()) {
            reject("positional decorator arguments are not supported");
            return;
        }
        for (auto& kw : dcall->keywords) {
            bool ok = (kw.arg == "init" || kw.arg == "repr" || kw.arg == "eq") &&
                      kw.value->kind == ExprKind::Constant &&
                      static_cast<const Constant*>(kw.value.get())->ck == Constant::CK::Bool &&
                      static_cast<const Constant*>(kw.value.get())->b;
            if (!ok) {
                reject("decorator option '" + kw.arg + "' is not supported "
                            "(frozen/order/kw_only/slots/unsafe_hash/field defaults)");
                return;
            }
        }
    }

    // ── v1 只支持单类（无基类）──
    if (!c.bases.empty()) {
        reject("dataclass inheritance is not supported");
        return;
    }

    // ── 字段检查 ──
    bool seen_default = false;
    for (auto& f : cls->fields) {
        if (f.name == "self" || f.name == "other" || f.name == "__pa_dc_o") {
            reject("field name '" + f.name + "' collides with synthesized method locals");
            return;
        }
        if (!dc_field_type_ok(f.ty)) {
            reject("field '" + f.name + "' has an unsupported annotation "
                        "(v1: int/float/bool/str/bytes/list/dict/tuple/set)");
            return;
        }
        if (f.has_default) {
            seen_default = true;
            const Expr* d = f.default_expr;
            if (!d) {
                reject("field '" + f.name + "' has no literal default");
                return;
            }
            if (d->kind == ExprKind::List || d->kind == ExprKind::Dict ||
                d->kind == ExprKind::Set) {
                reject("mutable default for field '" + f.name +
                            "' (use dataclasses.field(default_factory=...) — not supported in v1)");
                return;
            }
            if (d->kind == ExprKind::Call) {
                reject("dataclasses.field() / call defaults are not supported");
                return;
            }
            if (d->kind != ExprKind::Constant) {
                reject("non-literal default for field '" + f.name + "'");
                return;
            }
        } else if (seen_default) {
            reject("non-default argument '" + f.name + "' follows default argument");
            return;
        }
    }

    // ── 合成 __init__：逐字段赋值 + 字面量默认参 ──
    if (!body_has_method(c, "__init__")) {
        auto fn = std::make_unique<FunctionDef>();
        fn->name = "__init__";
        fn->line = line; fn->col = col;
        Arg self; self.name = "self";
        fn->args.args.push_back(std::move(self));
        for (auto& f : cls->fields) {
            Arg a;
            a.name = f.name;
            // 标注从字段 AnnAssign 克隆（类型已检查过，形态必在克隆器覆盖内）。
            const Expr* ann = nullptr;
            for (auto& s : c.body) {
                if (s->kind != StmtKind::AnnAssign) continue;
                auto& as = static_cast<AnnAssign&>(*s);
                if (as.target->kind == ExprKind::Name &&
                    static_cast<Name&>(*as.target).id == f.name) {
                    ann = as.annotation.get();
                    break;
                }
            }
            a.annotation = clone_annotation(ann);
            if (!a.annotation) {
                reject("field '" + f.name + "' annotation form is not supported");
                return;
            }
            fn->args.args.push_back(std::move(a));
            if (f.has_default)
                fn->args.defaults.push_back(clone_annotation(f.default_expr));
        }
        fn->returns = mk_name("None", line, col);
        for (auto& f : cls->fields) {
            auto as = std::make_unique<Assign>();
            as->line = line; as->col = col;
            as->targets.push_back(mk_attr(mk_name("self", line, col), f.name, line, col));
            as->value = mk_name(f.name, line, col);
            fn->body.push_back(std::move(as));
        }
        if (fn->body.empty()) {
            auto p = std::make_unique<Pass>();
            p->line = line; p->col = col;
            fn->body.push_back(std::move(p));
        }
        c.body.push_back(std::move(fn));
    }

    // ── 合成 __repr__："Name(f=repr(self.f), ...)" ──
    if (!body_has_method(c, "__repr__")) {
        auto fn = std::make_unique<FunctionDef>();
        fn->name = "__repr__";
        fn->line = line; fn->col = col;
        Arg self; self.name = "self";
        fn->args.args.push_back(std::move(self));
        fn->returns = mk_name("str", line, col);
        // CPython 的 dataclass repr 用 __qualname__：函数内定义的类是
        // func.<locals>.Cls（单层嵌套；更深层嵌套的完整链不在 v1 范围）。
        std::string qual = cur_fn_def_ ? cur_fn_def_->name + ".<locals>." + c.name
                                       : c.name;
        ExprPtr acc = mk_str(qual + "(", line, col);
        bool first = true;
        for (auto& f : cls->fields) {
            if (!first) acc = mk_add(std::move(acc), mk_str(", ", line, col), line, col);
            first = false;
            acc = mk_add(std::move(acc), mk_str(f.name + "=", line, col), line, col);
            auto repr_call = std::make_unique<Call>();
            repr_call->func = mk_name("repr", line, col);
            repr_call->args.push_back(mk_attr(mk_name("self", line, col), f.name, line, col));
            repr_call->line = line; repr_call->col = col;
            acc = mk_add(std::move(acc), std::move(repr_call), line, col);
        }
        acc = mk_add(std::move(acc), mk_str(")", line, col), line, col);
        auto ret = std::make_unique<Return>();
        ret->value = std::move(acc);
        ret->line = line; ret->col = col;
        fn->body.push_back(std::move(ret));
        c.body.push_back(std::move(fn));
    }

    // ── 合成 __eq__：isinstance 守卫 + 逐字段比较（异类 False）──
    if (!body_has_method(c, "__eq__")) {
        auto fn = std::make_unique<FunctionDef>();
        fn->name = "__eq__";
        fn->line = line; fn->col = col;
        Arg self; self.name = "self";
        fn->args.args.push_back(std::move(self));
        Arg other; other.name = "other";  // 不标注：unknown 域，isinstance 走运行期探针
        fn->args.args.push_back(std::move(other));
        fn->returns = mk_name("bool", line, col);

        auto guard = std::make_unique<Call>();
        guard->func = mk_name("isinstance", line, col);
        guard->args.push_back(mk_name("other", line, col));
        guard->args.push_back(mk_name(c.name, line, col));
        guard->line = line; guard->col = col;

        auto ifstmt = std::make_unique<If>();
        ifstmt->line = line; ifstmt->col = col;
        If::Branch br;
        br.cond = std::move(guard);
        // 守卫内把 other 绑成静态类类型，字段访问/容器逐元素比较才有静态路径
        // （unknown 域没有属性面，容器 == 会退化成指针比较）。
        auto bind = std::make_unique<AnnAssign>();
        bind->target = mk_name("__pa_dc_o", line, col);
        bind->annotation = mk_name(c.name, line, col);
        bind->value = mk_name("other", line, col);
        bind->line = line; bind->col = col;
        br.body.push_back(std::move(bind));
        auto ret = std::make_unique<Return>();
        ret->line = line; ret->col = col;
        if (cls->fields.empty()) {
            auto t = std::make_unique<Constant>();
            t->ck = Constant::CK::Bool; t->b = true;
            t->line = line; t->col = col;
            ret->value = std::move(t);
        } else {
            auto conj = std::make_unique<BoolOp>();
            conj->op = Tok::KW_AND;
            conj->line = line; conj->col = col;
            for (auto& f : cls->fields) {
                auto cmp = std::make_unique<Compare>();
                cmp->left = mk_attr(mk_name("self", line, col), f.name, line, col);
                cmp->ops.push_back(CmpOp::Eq);
                cmp->comparators.push_back(
                    mk_attr(mk_name("__pa_dc_o", line, col), f.name, line, col));
                cmp->line = line; cmp->col = col;
                conj->values.push_back(std::move(cmp));
            }
            ret->value = std::move(conj);
        }
        br.body.push_back(std::move(ret));
        ifstmt->branches.push_back(std::move(br));
        auto ret_false = std::make_unique<Return>();
        auto fc = std::make_unique<Constant>();
        fc->ck = Constant::CK::Bool; fc->b = false;
        fc->line = line; fc->col = col;
        ret_false->value = std::move(fc);
        ret_false->line = line; ret_false->col = col;
        ifstmt->orelse.push_back(std::move(ret_false));
        fn->body.push_back(std::move(ifstmt));
        c.body.push_back(std::move(fn));
    }
}

}  // namespace pyadamas
