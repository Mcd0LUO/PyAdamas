// typecheck/query.h —— 类型判定与显示（is_num/type_eq/type_name）
//
// typecheck/types.h 的组成部分（纯代码移动，声明顺序不变）：
// 由 typecheck/types.h 末尾统一包含，不要单独 include。
#pragma once

#include "typecheck/types.h"

namespace pyadamas {

// ── 判定与显示 ────────────────────────────────────────────────────

inline bool is_num(const TypePtr& t) {
    return t && (is_int_kind(t->kind) || t->kind == TyKind::Float ||
                 t->kind == TyKind::Bool || t->kind == TyKind::Big);
}
inline bool is_unknown(const TypePtr& t) { return !t || t->kind == TyKind::Unknown; }

inline bool type_eq(const TypePtr& a, const TypePtr& b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    if (a->kind == TyKind::Func) {
        if (!a->fn || !b->fn) return a->fn == b->fn;
        if (a->fn->pos.size() != b->fn->pos.size() ||
            a->fn->kwonly.size() != b->fn->kwonly.size())
            return false;
        for (size_t i = 0; i < a->fn->pos.size(); ++i)
            if (!type_eq(a->fn->pos[i].ty, b->fn->pos[i].ty)) return false;
        for (size_t i = 0; i < a->fn->kwonly.size(); ++i)
            if (!type_eq(a->fn->kwonly[i].ty, b->fn->kwonly[i].ty)) return false;
        return type_eq(a->fn->ret, b->fn->ret);
    }
    if (a->kind == TyKind::BoundMethod) return a->name == b->name && type_eq(a->sub[0], b->sub[0]);
    if (a->kind == TyKind::Class) return a->cls == b->cls;
    if (a->kind == TyKind::Struct || a->kind == TyKind::StructPtr) return a->st == b->st;
    if (a->kind == TyKind::Exception) return true;  // 异常值统一按 Exception 处理
    if (a->kind == TyKind::Tuple && a->variadic != b->variadic) return false;
    if (a->sub.size() != b->sub.size()) return false;
    for (size_t i = 0; i < a->sub.size(); ++i)
        if (!type_eq(a->sub[i], b->sub[i])) return false;
    return true;
}

inline std::string type_name_impl(const TypePtr& t, int depth) {
    if (!t) return "unknown";
    if (depth > 12) return "...";
    switch (t->kind) {
        case TyKind::Unknown: {
            if (!is_union_ty(t)) return "unknown";
            std::string out;
            for (auto& m : t->sub) {
                if (!out.empty()) out += " | ";
                out += type_name_impl(m, depth + 1);
            }
            return out;
        }
        case TyKind::None: return "None";
        case TyKind::Bool: return "bool";
        case TyKind::Int: return "int";
        case TyKind::I8: return "i8";
        case TyKind::I16: return "i16";
        case TyKind::I32: return "i32";
        case TyKind::U8: return "u8";
        case TyKind::U16: return "u16";
        case TyKind::U32: return "u32";
        case TyKind::U64: return "u64";
        case TyKind::Big: return "big";
        case TyKind::Float: return "float";
        case TyKind::Complex: return "complex";
        case TyKind::Str: return "str";
        case TyKind::Bytes: return "bytes";
        case TyKind::List:
            return t->sub.empty() ? "list"
                                  : std::format("list[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Range:
            return t->sub.empty() ? "range"
                                  : std::format("range[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Set:
            return t->sub.empty() ? "set"
                                  : std::format("set[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Dict:
            return t->sub.size() < 2
                       ? "dict"
                       : std::format("dict[{}, {}]", type_name_impl(t->sub[0], depth + 1),
                                     type_name_impl(t->sub[1], depth + 1));
        case TyKind::Optional:
            return std::format("{} | None",
                               t->sub.empty() ? "unknown" : type_name_impl(t->sub[0], depth + 1));
        case TyKind::Tuple: {
            if (t->variadic)
                return std::format("tuple[{}, ...]",
                                   t->sub.empty() ? "unknown" : type_name_impl(t->sub[0], depth + 1));
            std::string s = "tuple[";
            for (size_t i = 0; i < t->sub.size(); ++i) {
                if (i) s += ", ";
                s += type_name_impl(t->sub[i], depth + 1);
            }
            return s + "]";
        }
        case TyKind::Func: {
            if (!t->fn) return "func";
            std::string s = "func(";
            bool first = true;
            for (const auto& p : t->fn->pos) {
                if (!first) s += ", ";
                first = false;
                s += type_name_impl(p.ty, depth + 1);
                if (p.has_default) s += "=?";
            }
            if (t->fn->varargs) {
                if (!first) s += ", ";
                first = false;
                s += "*" + type_name_impl(t->fn->vararg_ty, depth + 1);
            }
            if (!t->fn->kwonly.empty() && !t->fn->varargs) {
                if (!first) s += ", ";
                s += "*";
                first = false;
            }
            for (const auto& p : t->fn->kwonly) {
                if (!first) s += ", ";
                first = false;
                s += type_name_impl(p.ty, depth + 1);
                if (p.has_default) s += "=?";
            }
            if (t->fn->kwargs) {
                if (!first) s += ", ";
                s += "**" + type_name_impl(t->fn->kwarg_ty, depth + 1);
            }
            s += ") -> ";
            s += t->fn->ret ? type_name_impl(t->fn->ret, depth + 1) : "None";
            return s;
        }
        case TyKind::BoundMethod:
            return std::format("method '{}' of {}", t->name,
                               t->sub.empty() ? "?" : type_name_impl(t->sub[0], depth + 1));
        case TyKind::Class:
            return t->cls ? t->cls->name : "class";
        case TyKind::Struct:
            return t->st ? t->st->name : "struct";
        case TyKind::StructPtr:
            return t->st ? std::format("ptr[{}]", t->st->name) : "ptr";
        case TyKind::Generator:
            return t->sub.empty()
                       ? "generator"
                       : std::format("generator[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Coroutine:
            return t->sub.empty()
                       ? "coroutine"
                       : std::format("coroutine[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Task:
            return t->sub.empty() ? "task"
                                  : std::format("task[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::AsyncGenerator:
            return t->sub.empty()
                       ? "asyncgen"
                       : std::format("asyncgen[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::Future:
            return t->sub.empty() ? "future"
                                  : std::format("future[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::TypeObj: return "type";
        case TyKind::ClassObj:
            return t->sub.empty() ? "type"
                                  : std::format("type[{}]", type_name_impl(t->sub[0], depth + 1));
        case TyKind::AsyncTimeout: return "asyncio.Timeout";
        case TyKind::AsyncTaskGroup: return "asyncio.TaskGroup";
        case TyKind::AsyncSemaphore: return "asyncio.Semaphore";
        case TyKind::AsyncEvent: return "asyncio.Event";
        case TyKind::AsyncLock: return "asyncio.Lock";
        case TyKind::AsyncQueue:
            return t->sub.empty()
                       ? "asyncio.Queue"
                       : std::format("asyncio.Queue[{}]",
                                     type_name_impl(t->sub[0], depth + 1));
        case TyKind::Exception:
            return t->name.empty() ? "Exception" : t->name;
        case TyKind::File: return "file";
        case TyKind::Random: return "random.Random";
        case TyKind::CompletedProcess: return "subprocess.CompletedProcess";
        case TyKind::StatResult: return "os.stat_result";
        case TyKind::StructTime: return "time.struct_time";
        case TyKind::UnameInfo: return "posix.uname_result";
        case TyKind::FloatInfo: return "sys.float_info";
        case TyKind::SysFlags: return "sys.flags";
        case TyKind::Popen: return "subprocess.Popen";
        case TyKind::Regex: return "re.Pattern";
        case TyKind::Match: return "re.Match";
        case TyKind::DateTime: return "datetime";
        case TyKind::TimeDelta: return "timedelta";
        case TyKind::Path: return "pathlib.Path";
        case TyKind::Deque: return "collections.deque";
        case TyKind::DYLIB: return "dylib";
        case TyKind::VThread: return "vthread";
        case TyKind::Chan:
            return t->sub.empty() ? "chan"
                                  : std::format("chan[{}]", type_name_impl(t->sub[0], depth + 1));
    }
    return "?";
}

inline std::string type_name(const TypePtr& t) { return type_name_impl(t, 0); }

// 错误消息里的类型名不带参数：CPython 说 'tuple'，不说 'tuple[int]'。
inline std::string bare_type_name(const TypePtr& t) {
    if (!t) return "unknown";
    switch (t->kind) {
        case TyKind::None: return "NoneType";
        case TyKind::Bool: return "bool";
        case TyKind::Int: return "int";
        case TyKind::Big: return "int";      // CPython 语义：big 是 int
        case TyKind::Float: return "float";
        case TyKind::Complex: return "complex";
        case TyKind::Str: return "str";
        case TyKind::Bytes: return "bytes";
        case TyKind::List: return "list";
        case TyKind::Range: return "range";
        case TyKind::Set: return "set";
        case TyKind::Dict: return "dict";
        case TyKind::Tuple: return "tuple";
        case TyKind::Generator: return "generator";
        default: return type_name_impl(t, 0);
    }
}


}  // namespace pyadamas
