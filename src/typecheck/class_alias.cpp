// typecheck/class_alias.cpp —— 类体的顺序求值：def 绑定名字与方法别名
//
// CPython 的类体按顺序执行，`def` 当场把函数对象绑进类命名空间，所以
// `__str__ = short` / `__new__ = _new` 这类「方法别名」写法很常见（CPython
// 3.14 标准库里 27 个文件因此被前端拒绝过，见 docs/frontend-fixes.md §1）。
//
// pyadamas 的方法不是一等值（`A.short` 在降级层明确拒绝），所以别名不能真的
// 复制函数对象，而是建成「同一实现的两个方法槽」：往 cls->methods 追加一份
// MethodInfo（共享 sig/call_sig/def、slot 用别名的），降级时 vtable 的两个槽
// 写同一份实现，调用点（含 dunder 分派与属性 getter）按别名查得到。
#include "typecheck/typecheck.h"

namespace pyadamas {

// 类体里**已经执行过**的 def：登记进类体命名空间（后面的类体语句看得见），
// 并记下它在 cls->methods 里的下标供别名解析。记下标而不是指针 —— 别名
// 条目会往同一个 vector 里追加，指针会失效。
void TypeChecker::class_body_register_def(const ClassInfoPtr& cls, FunctionDef& f) {
    for (size_t i = cls->methods.size(); i-- > 0;) {
        MethodInfo& cand = cls->methods[i];
        if (cand.def != &f) continue;
        // 自定义装饰器的方法：类体里的名字绑定的是装饰器链的结果，类型未知，
        // 不当成方法别名来源（也不登记类体命名空间）。
        if (cand.decorated) break;
        if (cand.sig) {
            TypePtr ft = new_ty(TyKind::Func);
            ft->fn = cand.sig;
            class_body_attrs_[f.name] = ft;
        }
        class_body_methods_[f.name] = static_cast<int>(i);
        break;
    }
}

// 类体的 `NAME = 前面的方法名`：建成方法别名。返回 true 表示这条语句已经
// 被消费（降级层靠 ClassInfo::alias_assign_stmts 跳过它）。
bool TypeChecker::class_body_method_alias(const ClassInfoPtr& cls, Stmt& s) {
    if (cls->is_enum) return false;
    auto& as = static_cast<Assign&>(s);
    if (as.targets.size() != 1 || as.targets[0]->kind != ExprKind::Name ||
        as.value->kind != ExprKind::Name)
        return false;
    auto& nm = static_cast<Name&>(*as.targets[0]);
    auto& vn = static_cast<Name&>(*as.value);
    auto mit = class_body_methods_.find(vn.id);
    if (mit == class_body_methods_.end() ||
        mit->second >= static_cast<int>(cls->methods.size()))
        return false;
    const MethodInfo& src = cls->methods[static_cast<size_t>(mit->second)];
    if (!src.sig || src.decorated) return false;
    if (src.kind != MethodKind::Normal && src.kind != MethodKind::Static &&
        src.kind != MethodKind::ClassMethod && src.kind != MethodKind::PropertyGetter)
        return false;
    MethodInfo am = src;
    am.name = nm.id;
    am.aliases.clear();
    am.slot = intern_method_slot(method_slot_key(nm.id, src.kind));
    if (src.call_sig) {
        am.call_sig = std::make_shared<FuncInfo>(*src.call_sig);
        am.call_sig->name = nm.id;
    }
    // @property 的别名：属性表也要指向同一个 getter，实例上的 obj.alias 才读得到。
    if (src.kind == MethodKind::PropertyGetter) {
        PropertyInfo pi;
        pi.owner = cls.get();
        pi.getter = static_cast<int>(cls->methods.size());
        cls->properties[nm.id] = pi;
    }
    // owner 保持**声明者**：alias 只是同一个实现的第二个名字，delta 仍按声明类算。
    cls->methods.push_back(std::move(am));
    if (auto ait = class_body_attrs_.find(vn.id); ait != class_body_attrs_.end())
        class_body_attrs_[nm.id] = ait->second;
    cls->alias_assign_stmts.insert(&s);
    return true;
}

}  // namespace pyadamas
