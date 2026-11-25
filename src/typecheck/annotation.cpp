// typecheck/annotation.cpp —— 注解（类型标注）解析
//
// 0.483 从 typecheck.cpp 拆出（规模检查 1200 行硬限）。三个入口都是
// TypeChecker 的成员：parse_annotation 是对外唯一入口（容器元素类型是硬约束），
// parse_annotation_impl 处理表达式形态，parse_annotation_text 处理字符串注解
// （typing.py 里到处是 Forward 字符串）。深度 8 的兜底在 impl 里。
#include "typecheck/typecheck.h"

#include "ir/module.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"

namespace pyadamas {

TypePtr TypeChecker::parse_annotation(Expr* e, int line, int depth) {
        auto t = parse_annotation_impl(e, line, depth);
        lock_type(t);  // 标注里的容器元素类型是硬约束
        return t;
    }

TypePtr TypeChecker::parse_annotation_impl(Expr* e, int line, int depth) {
        // 字符串注解会回到词法/语法层重新解析，内容里若又是字符串注解
        // 就会无限展开（typing.py 里到处是 "Forward"）。深度兜底。
        if (depth > 8) return t_unknown();
        if (!e) return nullptr;

        // 取类型名的最后一段：typing.List → List，collections.abc.Sequence → Sequence
        auto base_of = [](Expr* x) -> std::string {
            if (auto* n = dynamic_cast<Name*>(x)) return n->id;
            if (auto* a = dynamic_cast<Attribute*>(x)) return a->attr;
            return {};
        };

        if (auto* n = dynamic_cast<Name*>(e)) {
            const std::string& id = n->id;
            if (id == "int") return t_int();
            // ── 定宽整数标注（P1，docs/numeric-types-design.md §1.1）──
            // i64 与 int 是同一个类型（Int = i64 机器字）；long 是 C 口径的
            // i64 别名（x86-64 long = 64 位）。char = i8 别名（C 式字节，
            // x86 char 默认 signed），不是 Unicode 码点——码点用 i32/u32。
            if (id == "i64" || id == "long") return t_int();
            if (id == "i8" || id == "char") return t_i8();
            if (id == "i16" || id == "short") return t_i16();
            if (id == "i32") return t_i32();
            if (id == "u8" || id == "uchar") return t_u8();
            if (id == "u16" || id == "ushort") return t_u16();
            if (id == "u32" || id == "uint") return t_u32();
            if (id == "u64" || id == "ulong") return t_u64();
            if (id == "float") return t_float();
            // P3：big 任意精度整数（docs/numeric-types-design.md §1/§4）。
            if (id == "big") return t_big();
            // complex 有独立的 TyKind（P3 tagged union 里的 complex 子类），
            // 漏了它注解会静默退化成 unknown，调用方按整数打印指针。
            if (id == "complex") return t_complex();
            // 0.542：`buf: buffer` 这类标注（与内置 `buffer(n)` 返回同一个类型）。
            // 漏了它会静默退化成 unknown：字段读回是 unknown，`w.buf[i] = v` 走
            // 列表下标写路径 → 段错误 / 堆损坏。
            // 0.543：补全「有运行期实现但注解不认」的内置类型名。以前这些一律
            // 退化成 unknown，于是构造出来的值调方法/读字段都在降级期报
            // 「method/attribute is only supported ...」。
            //   deque（元素类型未知，deque[int] 走下标分支）
            //   datetime / date（同一个 TyKind::DateTime）
            //   timedelta / Path / range / Popen
            if (id == "deque" || id == "Deque") return mk_deque(t_unknown());
            if (id == "datetime" || id == "date") return new_ty(TyKind::DateTime);
            if (id == "timedelta") return new_ty(TyKind::TimeDelta);
            if (id == "Path") return new_ty(TyKind::Path);
            if (id == "range") return mk_range(t_int());
            if (id == "Popen") return t_popen();
            // 0.546：vthread 的通道。跨模块函数拿不到静态类型时 ch.send/recv 会在
            // 降级期报「not supported by the code generator yet」；裸名给 unknown
            // 元素类型（send 的值类型不检查），Chan[T] 走下标分支。
            if (id == "Chan") {
                auto ct = new_ty(TyKind::Chan);
                ct->sub = {t_unknown()};
                return ct;
            }
            if (id == "buffer") {
                auto bt = mk_buffer(t_float());
                bt->celem = CKind::Float;
                return bt;
            }
            if (id == "str") return t_str();
            if (id == "bytes") return t_bytes();
            if (id == "file") return t_file();
            if (id == "stat_result") return t_stat();
            if (id == "struct_time") return t_struct_time();
            if (id == "uname_result") return t_uname();
            if (id == "float_info") return t_float_info();
            if (id == "sys_flags") return t_sys_flags();
            if (id == "bool") return t_bool();
            if (id == "None") return t_none();
            // 只建模具体容器，不建模协议：Sequence/Iterable 这类标注
            // 允许任何可迭代对象，压成 list 会把 set/tuple 实参判成错误
            if (id == "Sequence" || id == "Iterable" || id == "Iterator" ||
                id == "Collection" || id == "Container" || id == "Reversible")
                return t_unknown();
            // **裸容器标注用 fresh_hole，不用 t_unknown()。**
            //
            // t_unknown() 是「推断失败」的全局单例，`fill_hole` 会显式跳过它
            // （原地写会把全程序的 unknown 一起改掉）。所以裸 `list` 写成
            // `mk_list(t_unknown())` 之后，**洞永远填不上** —— 参数停在
            // `list[unknown]`，比完全不标注（能靠调用点回填推出 list[int]）
            // 还差。
            //
            // fresh_hole() 是可变的、独占的 Unknown 实例，能被调用点实参或
            // 函数体用法填上。这才是「是 list，元素类型待推断」的正确表示。
            if (id == "list" || id == "List" || id == "MutableSequence")
                return mk_list(fresh_hole());
            if (id == "dict" || id == "Dict" || id == "Mapping" || id == "MutableMapping")
                return mk_dict(fresh_hole(), fresh_hole());
            if (id == "tuple" || id == "Tuple") return mk_tuple_rest(fresh_hole());
            if (id == "set" || id == "Set" || id == "FrozenSet" || id == "frozenset")
                return mk_set(fresh_hole());
            if (id == "object" || id == "Any") return t_unknown();
            if (auto it = classes_.find(id); it != classes_.end()) return mk_class(it->second.get());
            if (auto it = structs_.find(id); it != structs_.end()) return mk_struct(it->second);
            // 导入类写在标注里（`x: Base`）：按当前作用域里的类绑定解析。
            if (auto* sym = lookup(id);
                sym && sym->ty && sym->ty->kind == TyKind::Class)
                return sym->ty;
            // 前向引用或自造类型：不阻断检查
            return t_unknown();
        }
        if (auto* c = dynamic_cast<Constant*>(e)) {
            if (c->ck == Constant::CK::Str) return parse_annotation_text(c->s, line, depth + 1);
            if (c->ck == Constant::CK::None) return t_none();
            return t_unknown();
        }
        if (auto* s = dynamic_cast<Subscript*>(e)) {
            std::string base = base_of(s->value.get());
            std::vector<TypePtr> args;
            if (auto* tup = dynamic_cast<TupleExpr*>(s->slice.get())) {
                for (auto& el : tup->elts) args.push_back(parse_annotation(el.get(), line, depth));
            } else {
                args.push_back(parse_annotation(s->slice.get(), line, depth));
            }
            if (base == "Optional" && args.size() == 1) return mk_optional(args[0]);
            if ((base == "Future" || base == "Task" || base == "Coroutine" ||
                 base == "Generator" || base == "AsyncGenerator") &&
                !args.empty()) {
                TypePtr inner = args[0];
                if (base == "Future") return mk_future(inner);
                if (base == "Task") return mk_task(inner);
                if (base == "Coroutine") return mk_coroutine(inner);
                if (base == "Generator")
                    return mk_generator(inner, args.size() > 1 ? args[1] : nullptr,
                                        args.size() > 2 ? args[2] : nullptr);
                return mk_async_generator(inner);
            }
            if ((base == "deque" || base == "Deque") && args.size() == 1)
                return mk_deque(args[0] ? args[0] : t_unknown());
            // 0.546：Chan[T] —— 元素类型用于 send 的值类型检查。
            if (base == "Chan" && args.size() == 1) {
                auto ct = new_ty(TyKind::Chan);
                ct->sub = {args[0] ? args[0] : t_unknown()};
                return ct;
            }
            if (base == "Queue" && args.size() == 1)
                return mk_async_queue(args[0]);
            if (base == "Union") {
                TypePtr acc = nullptr;
                bool has_none = false;
                for (auto& a : args) {
                    if (a && a->kind == TyKind::None) has_none = true;
                    else acc = acc ? join(acc, a) : a;
                }
                if (has_none) return mk_optional(acc ? acc : t_unknown());
                return acc ? acc : t_unknown();
            }
            if ((base == "Sequence" || base == "Iterable" || base == "Iterator" ||
                 base == "Collection" || base == "Container" || base == "Reversible") &&
                args.size() == 1)
                return t_unknown();
            // **裸 `list` / `set` / `dict` 标注：元素类型用 fresh_hole，
            // 让后续的赋值/append/return 把它填上，而不是直接判成 unknown。**
            //
            // 原来只有带参数的形式（`list[int]`）才被识别，裸 `list` 落到
            // 函数末尾 `return t_unknown()` —— 于是 `def make(n: int) -> list:`
            // 的函数返回类型是 **unknown**，比完全不标注（能推断出 list[int]）
            // 还差。
            //
            // 实测 bench/compare/matmul.py：`def make(n: int) -> list:` 让
            // a/b/c 全退化成 list[unknown]，最内层循环的 index.get 变成
            // unknown 路径、+= 走 pa_list_prep 的保守分支，11ms；
            // 改成 `-> list[int]` 后 7ms（-36%）。裸 `list` 应该等价于
            // 「是 list，元素类型待推断」，不能当成 unknown。
            if (base == "list" || base == "List" || base == "MutableSequence" ||
                base == "Sequence" || base == "Iterable") {
                if (args.size() == 1) return mk_list(args[0]);
                if (args.empty()) return mk_list(fresh_hole());
            }
            if (base == "set" || base == "Set" || base == "FrozenSet" || base == "frozenset") {
                if (args.size() == 1) return mk_set(args[0]);
                if (args.empty()) return mk_set(fresh_hole());
            }
            if (base == "dict" || base == "Dict" || base == "Mapping" ||
                base == "MutableMapping") {
                if (args.size() == 2) return mk_dict(args[0], args[1]);
                if (args.empty()) return mk_dict(fresh_hole(), fresh_hole());
            }
            if (base == "tuple" || base == "Tuple") {
                // tuple[int, ...]：省略号表示变长，元素类型只有 int
                if (auto* tup = dynamic_cast<TupleExpr*>(s->slice.get());
                    tup && tup->elts.size() == 2) {
                    auto* c = dynamic_cast<Constant*>(tup->elts[1].get());
                    if (c && c->ck == Constant::CK::Ellipsis)
                        return mk_tuple_rest(parse_annotation(tup->elts[0].get(), line, depth));
                }
                // 裸 tuple：长度和元素都未知
                if (args.empty()) return mk_tuple_rest(t_unknown());
                return mk_tuple(std::move(args));
            }
            // ptr[int] 这类 C 指针标注：类型仍是 Optional[int]，但保留元素
            // 种类，函数体内可以 a[0] 解引用。
            if (base == "ptr" && args.size() == 1) {
                auto t = mk_optional(t_int());
                if (args[0] && args[0]->kind == TyKind::Float) t->celem = CKind::Float;
                else if (args[0] && args[0]->kind == TyKind::Bool) t->celem = CKind::Bool;
                else t->celem = CKind::Int;
                return t;
            }
            return t_unknown();
        }
        if (auto* at = dynamic_cast<Attribute*>(e)) {
            if (auto* vn = dynamic_cast<Name*>(at->value.get())) {
                if (auto mod = lookup_module_alias(vn->id); mod) {
                    if (mod->builtin) {
                        if (mod->name == "random" && at->attr == "Random") return t_random();
                        if (mod->name == "subprocess" && at->attr == "CompletedProcess")
                            return t_completed();
                        if (mod->name == "subprocess" && at->attr == "Popen") return t_popen();
                        // 0.543：`re.Pattern` / `re.Match` 的 dotted 注解。
                        if (mod->name == "re" && at->attr == "Pattern") return t_regex();
                        if (mod->name == "re" && at->attr == "Match") return t_match();
                        // B3b（0.533）：asyncio 原语的内建类型。标注成 unknown 会让
                        // `async with` 在**降级**阶段被拒（前端放行、后端报错），
                        // 而不标注反而能过 —— 标注了更差。
                        if (mod->name == "asyncio") {
                            if (at->attr == "Lock") return mk_async_lock();
                            if (at->attr == "Semaphore") return mk_async_semaphore();
                            if (at->attr == "Event") return mk_async_event();
                            if (at->attr == "Queue") return mk_async_queue(t_unknown());
                        }
                        // 0.550：内置模块里**用 pya 源码实现**的类（`io.StringIO` /
                        // `io.BytesIO` 就在 stdlib/io.py 里）。以前这一支只认上面那几个
                        // 硬编码名字，其余 dotted 注解一律退化成 unknown，于是
                        // `s: io.StringIO = io.StringIO()` 之后 `s.write/getvalue`
                        // 全在降级期报「method ... is not supported」。
                        if (auto ci = mod->classes.find(at->attr);
                            ci != mod->classes.end() && ci->second.cls)
                            return mk_class(ci->second.cls.get());
                    } else {
                        // 跨模块类标注 `p: a.Point`：接口里带完整字段/方法表，
                        // 解析成同一个 ClassInfo*（lowering 走 imported class 路径）。
                        if (auto ci = mod->classes.find(at->attr); ci != mod->classes.end())
                            return mk_class(ci->second.cls.get());
                        if (auto it = mod->globals.find(at->attr); it != mod->globals.end())
                            return it->second;
                    }
                }
            }
        }
        if (auto* b = dynamic_cast<BinOp*>(e); b && b->op == Tok::VBAR) {
            // int | None 这类联合类型
            auto l = parse_annotation(b->left.get(), line, depth);
            auto r = parse_annotation(b->right.get(), line, depth);
            if (l && l->kind == TyKind::None) return mk_optional(r);
            if (r && r->kind == TyKind::None) return mk_optional(l);
            if (l && l->kind == TyKind::Optional) return mk_optional(join(l->sub[0], r));
            if (r && r->kind == TyKind::Optional) return mk_optional(join(l, r->sub[0]));
            // 0.488（修 Bug A）：数值塔内部照旧走 join（int | float → float、
            // int | big → big，隐式提升语义不变）；join 合不了的成员组合以前
            // 会塌成 unknown（于是 float 也能赋进来），现在保留成联合类型。
            // 任一侧已经是联合：直接按成员表合并（不能走 join —— join 会把
            // 非单例 Unknown 当洞/信息丢失处理，成员会被顶掉）。
            if (is_union_ty(l) || is_union_ty(r)) return mk_union(l, r);
            auto j = join(l, r);
            if (is_unknown(j) && l && r && !is_unknown(l) && !is_unknown(r))
                return mk_union(l, r);
            return j;
        }
        return t_unknown();
    }

TypePtr TypeChecker::parse_annotation_text(const std::string& text, int line, int depth) {
        try {
            Lexer lx(text);
            Parser ps(lx.run());
            auto e = ps.parse_fstring_expr();
            return parse_annotation(e.get(), line, depth);
        } catch (const std::exception&) {
            return t_unknown();
        }
    }

}  // namespace pyadamas
