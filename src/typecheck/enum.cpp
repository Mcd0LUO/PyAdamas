// typecheck/enum.cpp —— enum / abc 的编译期类语义
//
// enum：`class Color(Enum): RED = 1` 的类体赋值变成枚举成员（编译期登记；
// 降级阶段在类创建时生成成员对象，见 lower/class.cpp）。
// abc：`@abstractmethod` 标记抽象方法，实例化时检查（unimplemented_abstract）。
// 从 typecheck/func.cpp 拆出来是为了守住文件规模闸门（tests/check_size.sh）。
#include "typecheck/typecheck.h"

namespace pyadamas {

void TypeChecker::mark_abstract_methods(const FunctionDef& f, MethodInfo& m) {
    for (auto& d : f.decorators) {
        if (!d) continue;
        const Expr* e = d.get();
        const std::string* dn = nullptr;
        if (e->kind == ExprKind::Name)
            dn = &static_cast<const Name*>(e)->id;
        else if (e->kind == ExprKind::Attribute)
            dn = &static_cast<const Attribute*>(e)->attr;
        else if (e->kind == ExprKind::Call) {
            const Expr* fe = static_cast<const Call*>(e)->func.get();
            if (fe && fe->kind == ExprKind::Name)
                dn = &static_cast<const Name*>(fe)->id;
            else if (fe && fe->kind == ExprKind::Attribute)
                dn = &static_cast<const Attribute*>(fe)->attr;
        }
        if (dn && (*dn == "abstractmethod" || *dn == "abstractclassmethod" ||
                   *dn == "abstractstaticmethod"))
            m.is_abstract = true;
    }
}

void TypeChecker::enum_member_assign(ClassInfo* cls, const std::string& name, const Expr* value,
                                     int line, int col) {
    auto lit = literal_of_expr(value);
    if (!lit || (lit->kind != LiteralValue::Kind::Int && lit->kind != LiteralValue::Kind::Str)) {
        err(line, col,
            std::format("enum member '{}' must have an int or str literal value "
                        "(auto()/expressions are not supported yet)", name));
        return;
    }
    for (auto& em : cls->enum_members) {
        if (em.name == name) {
            err(line, col, std::format("duplicate enum member '{}'", name));
            return;
        }
        const bool same = em.val.kind == lit->kind &&
                          ((lit->kind == LiteralValue::Kind::Int && em.val.i == lit->i &&
                            em.val.u64 == lit->u64) ||
                           (lit->kind == LiteralValue::Kind::Str && em.val.s == lit->s));
        if (same) {
            err(line, col,
                std::format("enum member '{}' reuses an existing value "
                            "(aliases are not supported yet)", name));
            return;
        }
    }
    cls->enum_members.push_back(EnumMember{name, *lit, line});
    // 类属性 `Color.RED` 的静态类型 = 本类实例。
    ClassAttrInfo ai;
    ai.name = name;
    ai.ty = mk_class(cls);
    ai.init = nullptr;
    ai.owner = cls;
    class_body_attrs_[name] = ai.ty;
    cls->class_attrs.push_back(std::move(ai));
}

// P1：enum 构造的实参校验（本模块 Color(...) 与跨模块 mod.Color(...) 共用）。
// 以前实参只求值不检查：给 int 值枚举传 str 能编过，降级层的 coerce 静默放过，
// 比较指令按 str 语义生成（pa_str_eq），运行期读串头直接段错误。
// 形状（恰好一个位置实参）与值类型（int 值枚举只收 int-like）都在这里判；
// unknown 放行（静态不知道），str 值枚举的实参不在这里判（降级层明确拒绝）。
void TypeChecker::check_enum_call(const ClassInfo* cls, Call& c) {
    std::vector<TypePtr> ats;
    ats.reserve(c.args.size());
    for (auto& arg : c.args) ats.push_back(expr(*arg));
    for (auto& kw : c.keywords) expr(*kw.value);
    if (c.args.size() != 1 || !c.keywords.empty()) {
        err(c.line, c.col,
            std::format("enum '{}' takes exactly one value argument", cls->name));
        return;
    }
    const FieldInfo* vfl = cls->field("value");
    if (!vfl || !vfl->ty || vfl->ty->kind != TyKind::Int) return;  // str 值枚举另判
    TypePtr at = ats.empty() ? nullptr : ats[0];
    if (at && !is_unknown(at) && !is_int_like(at))
        err(c.args[0]->line, c.args[0]->col,
            std::format("enum '{}' expects an int value, got '{}'", cls->name,
                        type_name(at)));
}

void TypeChecker::finalize_enum(const ClassDef& c, ClassInfo* cls) {
    if (!cls->fields.empty())
        err(c.line, c.col, std::format("enum class '{}' cannot declare fields", cls->name));
    bool all_int = true, all_str = true;
    for (auto& em : cls->enum_members) {
        if (em.val.kind != LiteralValue::Kind::Int) all_int = false;
        if (em.val.kind != LiteralValue::Kind::Str) all_str = false;
    }
    if (!cls->enum_members.empty() && !all_int && !all_str)
        err(c.line, c.col,
            std::format("enum class '{}': mixed int/str member values are not supported yet",
                        cls->name));
    FieldInfo vf;
    vf.name = "value";
    vf.ty = all_str ? t_str() : t_int();
    FieldInfo nf;
    nf.name = "name";
    nf.ty = t_str();
    cls->fields.push_back(std::move(vf));
    cls->fields.push_back(std::move(nf));
    for (auto& em : cls->enum_members)
        if (cls->method(em.name) || cls->property(em.name))
            err(em.line, 0,
                std::format("enum member '{}' conflicts with a method/property", em.name));
}

}  // namespace pyadamas
