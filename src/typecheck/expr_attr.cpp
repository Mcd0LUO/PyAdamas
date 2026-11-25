// typecheck/expr_attr.cpp —— 属性表达式的类型检查（从 typecheck/expr.cpp 拆出）
//
// 文件规模硬限制 1200 行：P4 语言缺口给 typecheck/expr.cpp 加了异常类当值
// 的类型分支后超限，把最大的 Attribute case 按职责拆到这里。逻辑逐字搬移，
// 不改语义（门控关时 asm 逐字节不变的闸门要求）。
#include "driver/build.h"
#include "typecheck/typecheck.h"

#include "ir/module.h"

namespace pyadamas {

TypePtr TypeChecker::expr_attribute(Attribute& a) {
    if (auto* vn = dynamic_cast<Name*>(a.value.get())) {
        if (const CImportDecl* cd = cmodule(vn->id)) {
            for (auto& c : cd->consts)
                if (c.name == a.attr) return ckind_type(c.kind);
            for (auto& v : cd->vars) {
                if (v.py_name != a.attr) continue;
                if (v.kind == CKind::Ptr && !v.struct_name.empty()) {
                    if (StructInfo* st = struct_info(v.struct_name))
                        return mk_optional(mk_struct_ptr(st));
                }
                return ckind_type(v.kind);
            }
            for (auto& x : cd->externs) {
                if (x.py_name != a.attr) continue;
                auto it = extern_fns_.find(&x);
                if (it == extern_fns_.end()) break;
                auto ft = new_ty(TyKind::Func);
                ft->fn = it->second;
                return ft;
            }
            err(a.line, a.col,
                std::format("cimport '{}' has no member '{}'", vn->id, a.attr));
            return t_unknown();
        }
    }
    if (auto* vn = dynamic_cast<Name*>(a.value.get())) {
        if (auto mod = lookup_module_alias(vn->id)) {
            // CPython 桥代理：先验阶段只支持调用（call_forms 里
            // 放行成 unknown）。裸属性读（math.pi / sys.version）
            // 需要 getattr 帧，明确拒绝，见设计文档后续工作。
            if (mod->cpy_proxy) {
                err(a.line, a.col,
                    std::format("CPython module proxy '{}' supports calls only "
                                "in the prototype (attribute reads are future work)",
                                vn->id));
                return t_unknown();
            }
            if (auto ci = mod->classes.find(a.attr); ci != mod->classes.end())
                return mk_class_obj(mk_class(ci->second.cls.get()));
            auto it = mod->globals.find(a.attr);
            if (it == mod->globals.end()) {
                if (auto msg = reject_message(vn->id, a.attr); !msg.empty()) {
                    err(a.line, a.col, msg);
                    return t_unknown();
                }
                err(a.line, a.col,
                    std::format("module '{}' has no attribute '{}'", vn->id, a.attr));
                return t_unknown();
            }
            return it->second;
        }
    }
    // str.capitalize / int.bit_length 这类「类型上的未绑定方法」
    if (auto* vn = dynamic_cast<Name*>(a.value.get()); vn && is_type_name(vn->id))
        return t_unknown();
    auto ot = expr(*a.value);

    // 属性收窄（0.498）：`if self.x is not None:` 之后 `self.x` 直接用窄化后的类型。
    if (auto* bn = dynamic_cast<Name*>(a.value.get())) {
        auto it = attr_refine_.find(bn->id + "." + a.attr);
        if (it != attr_refine_.end()) return it->second;
    }
    if (is_unknown(ot)) return t_unknown();
    if (ot->kind == TyKind::Optional) {
        // 0.498：窄化之后走到这里说明没判空 —— 报错给出可操作提示，
        // 然后仍按内层类型继续检查（避免一条语句刷两条错）。
        // C 结构体/联合指针的可空性（cimport 返回 T | None）不是 Python None
        // 语义：运行期是裸指针，判空写法在 C 代码里很少见。这里只解包不报错，
        // 否则 tests/run/104_cstruct / 215_c_float32 / 527_union_array 会被误杀。
        const TyKind ik0 = ot->sub.empty() ? TyKind::Unknown : ot->sub[0]->kind;
        if (ik0 != TyKind::Struct && ik0 != TyKind::StructPtr)
            maybe_none(a.value.get(), ot, a.line, a.col);
        // 收窄做不到逐条路径成立，但内层是类/结构体指针时字段
        // 类型是确定的；运行时有空指针检查兜底。
        if (ot->sub.empty()) return t_unknown();
        TyKind ik = ot->sub[0]->kind;
        if (ik != TyKind::Class && ik != TyKind::Struct && ik != TyKind::StructPtr)
            return t_unknown();
        ot = ot->sub[0];
    }
    if (ot->kind == TyKind::ClassObj) {
        if (a.attr == "__name__") return t_str();
        if (!ot->sub.empty()) ot = ot->sub[0];
    }
    if (ot->kind == TyKind::Struct || ot->kind == TyKind::StructPtr) {
        const StructInfo* s = ot->st;
        if (!s) return t_unknown();
        if (const auto* f = s->field(a.attr)) {
            // .cpya 定长数组字段：属性读取得的是「元素地址视图」，不是元素值。
            if (f->is_array()) return c_array_view_type(*f);
            // 按值嵌套的 struct/union 字段：C 侧是内联布局，但 PyAdamas 侧
            // 一律按地址访问（返回字段地址，交给 StructPtr 路径读写字段）。
            if (f->ty && f->ty->kind == TyKind::Struct && f->ty->st)
                return mk_optional(mk_struct_ptr(f->ty->st));
            return f->ty;
        }
        err(a.line, a.col,
            std::format("struct '{}' has no field '{}'", s->name, a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::Exception) {
        if (a.attr == "args") return mk_tuple({t_str()});
        if (a.attr == "__cause__")
            return mk_optional(mk_exception("Exception"));
        // 内置异常的其它属性（OSError.errno、UnicodeError.code……）
        // 静态看不见，check 保守放过，build 再决定。
        unsupported.push_back("attribute on a builtin exception");
        return t_unknown();
    }
    if (ot->kind == TyKind::TypeObj || ot->kind == TyKind::ClassObj) {
        // type 对象的元数据属性：__name__/__qualname__/__doc__/
        // __module__ 是 str，__mro__/__bases__ 是元组，其余保守放行。
        if (a.attr == "__name__" || a.attr == "__qualname__" ||
            a.attr == "__doc__" || a.attr == "__module__" ||
            a.attr == "__dict__")
            return t_str();
        if (a.attr == "__mro__" || a.attr == "__bases__" ||
            a.attr == "__getstate__" || a.attr == "__length_hint__")
            return t_unknown();
        // 未知的 type 属性（像 _fields_ 这类动态的）保守放行。
        unsupported.push_back("attribute on a type object");
        return t_unknown();
    }
    if (ot->kind == TyKind::File) {
        if (a.attr == "closed") return t_bool();
        if (a.attr == "name" || a.attr == "mode" || a.attr == "encoding" ||
            a.attr == "errors")
            return t_str();
        // 方法名当值取用（file.write 不调用直接传）是文件
        // 缓存句柄的常见写法；返回 unknown 保守放行。
        if (a.attr == "write" || a.attr == "read" || a.attr == "readline" ||
            a.attr == "readlines" || a.attr == "readinto" || a.attr == "close" ||
            a.attr == "flush" || a.attr == "seek" || a.attr == "tell" ||
            a.attr == "truncate" || a.attr == "writelines" || a.attr == "fileno" ||
            a.attr == "isatty")
            return t_unknown();
        err(a.line, a.col,
            std::format("'file' object has no attribute '{}'", a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::DateTime) {
        if (a.attr == "year" || a.attr == "month" || a.attr == "day" ||
            a.attr == "hour" || a.attr == "minute" || a.attr == "second")
            return t_int();
        err(a.line, a.col,
            std::format("'datetime' object has no attribute '{}'", a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::TimeDelta) {
        if (a.attr == "days" || a.attr == "seconds" ||
            a.attr == "microseconds")
            return t_int();
        err(a.line, a.col,
            std::format("'timedelta' object has no attribute '{}'", a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::Path) {
        if (a.attr == "name" || a.attr == "stem" || a.attr == "suffix")
            return t_str();
        if (a.attr == "parent") return new_ty(TyKind::Path);
        if (a.attr == "parts") return mk_list(t_str());
        err(a.line, a.col,
            std::format("'pathlib.Path' object has no attribute '{}'", a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::Deque) {
        if (a.attr == "maxlen") return t_int();
        err(a.line, a.col,
            std::format("'collections.deque' object has no attribute '{}'",
                        a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::CompletedProcess) {
        if (a.attr == "returncode") return t_int();
        if (a.attr == "stdout" || a.attr == "stderr") return t_str();
        err(a.line, a.col,
            std::format("'CompletedProcess' object has no attribute '{}'", a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::StatResult) {
        // os.stat_result：10 个整数属性（CPython 的 tuple 顺序）。
        static const char* kStatFields[] = {"st_mode",  "st_ino",  "st_dev",   "st_nlink",
                                            "st_uid",   "st_gid",  "st_size",  "st_atime",
                                            "st_mtime", "st_ctime"};
        for (const char* f : kStatFields)
            if (a.attr == f) return t_int();
        err(a.line, a.col,
            std::format("'os.stat_result' object has no attribute '{}'", a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::Popen) {
        if (a.attr == "pid" || a.attr == "returncode") return t_int();
        if (a.attr == "stdout" || a.attr == "stderr" || a.attr == "stdin")
            return t_file();
        err(a.line, a.col,
            std::format("'Popen' object has no attribute '{}'", a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::Regex) {
        if (a.attr == "groups" || a.attr == "flags") return t_int();
        if (a.attr == "pattern") return t_str();
        err(a.line, a.col,
            std::format("re.Pattern has no attribute '{}'", a.attr));
        return t_unknown();
    }
    if (ot->kind == TyKind::Match) {
        if (a.attr == "lastindex") return t_int();
        if (a.attr == "lastgroup") return t_str();
        err(a.line, a.col, std::format("re.Match has no attribute '{}'", a.attr));
        return t_unknown();
    }
    // P4（lang_gaps）：异常类值的 __name__（运行期类对象单例）。
    if (lang_gaps_ && ot && ot->kind == TyKind::Class && ot->cls &&
        ot->cls->is_exception_class && a.attr == "__name__")
        return t_str();
    // 双下划线属性属于运行时的协议实现，静态上没有方法表
    if (a.attr.size() > 4 && a.attr.starts_with("__") && a.attr.ends_with("__"))
        return t_unknown();
    // Python 里函数是普通对象，可以挂任意属性（functools.wraps 就这么做）
    if (ot->kind == TyKind::Func) return t_unknown();
    if (ot->kind == TyKind::Class) {
        if (!ot->cls) return t_unknown();
        // 骨架：类体还没检查到（类定义在引用点之后），成员表是空的。
        // 正在检查本类方法时不算骨架——那时字段和方法都已登记完。
        if (!ot->cls->complete && ot->cls != cur_class_) return t_unknown();
        if (const PropertyInfo* p = ot->cls->property(a.attr)) {
            if (!p->owner || p->getter < 0 ||
                p->getter >= static_cast<int>(p->owner->methods.size()))
                return t_unknown();
            const MethodInfo& gm = p->owner->methods[p->getter];
            return gm.call_sig && gm.call_sig->ret ? gm.call_sig->ret
                                                   : t_unknown();
        }
        if (is_class_receiver(a)) {
            // 类接收者：`C.x` / `cls.x` 只可能是类属性或方法，
            // 实例字段在类上不存在（同名时也不能被它挡住）。
            if (const ClassAttrInfo* ca = ot->cls->class_attr(a.attr)) return ca->ty;
            if (const MethodInfo* m = ot->cls->method(a.attr))
                return mk_method_value(*m, true);
            if (ot->cls->is_open()) return t_unknown();
            err(a.line, a.col,
                std::format("class '{}' has no attribute '{}'", ot->cls->name,
                            a.attr));
            return t_unknown();
        }
        if (auto* f = ot->cls->field(a.attr)) {
            mark_self_field(a);  // 3.9：读 self.<字段> 记在方法所属类上
            return f->ty;
        }
        // 装饰方法在类属性里也有包装函数，必须先按方法查，
        // 否则会把含 self 的包装签名当成方法值的类型。
        if (const MethodInfo* m = ot->cls->method(a.attr))
            return mk_method_value(*m, false);
        if (const ClassAttrInfo* ca = ot->cls->class_attr(a.attr)) return ca->ty;
        // `self.x` 找不到：可能是 mixin 组合出来的成员，也可能
        // 是类体里赋的类属性——两种都不是错误，静态上认输。
        if (is_self_attr(a)) return t_unknown();
        // 有外部基类时成员表本来就不全，查不到不等于不存在
        if (ot->cls->is_open()) return t_unknown();
        // 类已因 setattr/hasattr/getattr 或非字面量名字开启了动态属性：
        // 读取落到运行期 dict。注意不能因为一次普通读取就 self-mark，
        // 否则拼错的字段名会被当成动态属性静默放过。
        if (ot->cls->needs_dict) return t_unknown();
        err(a.line, a.col,
            std::format("'{}' object has no attribute '{}'", ot->cls->name, a.attr));
        return t_unknown();
    }
    // 属性（不是方法）不能当可调用对象用：int.bit_length 是方法，
    // int.numerator 是属性，`n.numerator` 取到的是数值本身
    if (is_property_name(a.attr) && has_method(ot, a.attr)) return t_unknown();
    if (has_method(ot, a.attr)) return mk_bound(ot, a.attr);
    if (ot->kind == TyKind::None) {
        // None 接收者的属性多是 flow-narrowing 丢失；conservative。
        return t_unknown();
    }
    err(a.line, a.col,
        std::format("'{}' object has no attribute '{}'", type_name(ot), a.attr));
    return t_unknown();
}

}  // namespace pyadamas
