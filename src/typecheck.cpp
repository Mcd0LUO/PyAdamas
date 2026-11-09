// 由 src/typecheck.h 拆出的实现。声明见 /tmp/tc.h。
#include "typecheck.h"

#include "module.h"

namespace py23 {

void TypeChecker::check(Module& m) {
        auto mod = std::make_unique<Scope>();
        mod->is_module = true;
        module_ = mod.get();
        cur_ = module_;
        pool_.push_back(std::move(mod));

        collect_toplevel(m.body);
        hoist_module_bindings(m.body);
        hoist_global_decls(m.body);
        for (auto& s : m.body) stmt(*s);
    }

std::vector<std::pair<std::string, std::string>> TypeChecker::top_symbols() const {
        std::vector<std::pair<std::string, std::string>> out;
        if (!module_) return out;
        for (auto& [name, sym] : module_->vars)
            out.emplace_back(name, type_name(sym.ty));
        std::ranges::sort(out, [](auto& a, auto& b) { return a.first < b.first; });
        return out;
    }

Symbol* TypeChecker::lookup(const std::string& name) {
        for (Scope* s = cur_; s; s = s->parent) {
            auto it = s->vars.find(name);
            if (it == s->vars.end()) continue;
            // 命中祖先函数作用域 = 闭包捕获。模块 / 推导式 / 类作用域
            // 的 fn_owner 是空的，不算捕获。
            if (cur_fn_def_ && s->fn_owner && s->fn_owner != cur_fn_def_)
                free_refs_[cur_fn_def_].insert(name);
            return &it->second;
        }
        return nullptr;
    }

ModuleInfoPtr TypeChecker::lookup_module_alias(const std::string& name) const {
        for (Scope* s = cur_; s; s = s->parent) {
            // 普通变量可以遮蔽同名的模块别名（Python 里 import 也只是
            // 一次普通赋值）；先看 vars 再看 modules。
            if (s->vars.count(name)) return nullptr;
            auto it = s->modules.find(name);
            if (it != s->modules.end()) return it->second;
        }
        return nullptr;
    }

Scope* TypeChecker::assign_target_scope(const std::string& name) {
        if (cur_ == module_) return module_;
        if (global_decl_.count(name)) return module_;
        if (nonlocal_decl_.count(name)) {
            for (Scope* s = cur_fn_scope_ ? cur_fn_scope_->parent : cur_->parent; s; s = s->parent) {
                if (s->is_module) break;
                if (s->vars.count(name)) return s;
            }
            return cur_;  // 声明阶段已经报过「no binding」
        }
        return cur_;
    }

void TypeChecker::declare(const std::string& name, TypePtr ty, int line, int col, bool annotated) {
        // `x = None` 只是占位或清空，不是「这个槽位就是 None」。
        // 让它参与收窄会把 `loader_cls = None` 之前推出来的类型抹掉，
        // 也会把「先置 None、分支里再赋值」的代码判成 None 属性访问。
        bool assigning_none = ty && ty->kind == TyKind::None;
        Scope* sc = assign_target_scope(name);
        auto it = sc->vars.find(name);
        if (it == sc->vars.end()) {
            // 模块级 None 占位：函数体读它时，实际值取决于调用时机
            // （mimetypes 的 init() 就是通过 global 重新赋值），只能当 unknown
            if (assigning_none && sc->is_module) ty = t_unknown();
            sc->vars[name] = Symbol{std::move(ty), line, annotated, false};
            return;
        }
        // 同作用域重复赋值：类型必须保持一致
        Symbol& sym = it->second;
        if (annotated && !sym.annotated) {
            // 预扫描（hoist_scope_bindings）会先用「无标注」给模块级名字占位。
            // 真正的 `x: int = ...` 到这里才出现，必须把符号升级成有标注，
            // 否则后面 `x = "s"` 会走「无标注可改类型」的分支，静默变成 unknown。
            sym.ty = std::move(ty);
            sym.annotated = true;
            return;
        }
        if (sym.annotated) {
            // 函数名被重新绑定成静态上未知的东西（协程对象、装饰器返回值）：
            // 继续按原签名检查只会误报，直接退化成 unknown
            if (sym.from_def && is_unknown(ty)) {
                sym.ty = t_unknown();
                return;
            }
            // 给有标注的变量赋 None 当「清空」用，这是 Python 里的常见写法，
            // 真当成类型错误会大面积误杀；保持原类型，不收窄也不报错。
            if (!assigning_none && !assignable(sym.ty, ty))
                err(line, col,
                    std::format("cannot assign '{}' to '{}' variable '{}'", type_name(ty),
                                type_name(sym.ty), name));
            return;
        }
        if (assigning_none) return;  // 保持已有类型，不清空也不收窄
        // 无标注变量按 Python 习惯可以改类型：x = None 只是占位，
        // x = [] 的元素类型待定。取公共上界；合不上就退化成 unknown，
        // 后续检查自动放行 —— 宁可漏报，不可误杀。
        if (sym.ty && sym.ty->kind == TyKind::None) { sym.ty = ty; return; }
        if (is_unknown(sym.ty)) { sym.ty = ty; return; }
        if (assignable(sym.ty, ty)) {
            // 来源 unknown 时不要死守旧类型：变量可能已经换了一种东西
            if (is_unknown(ty)) sym.ty = t_unknown();
            return;
        }
        sym.ty = join(sym.ty, ty);
    }

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
            if (id == "float") return t_float();
            if (id == "str") return t_str();
            if (id == "bool") return t_bool();
            if (id == "None") return t_none();
            // 只建模具体容器，不建模协议：Sequence/Iterable 这类标注
            // 允许任何可迭代对象，压成 list 会把 set/tuple 实参判成错误
            if (id == "Sequence" || id == "Iterable" || id == "Iterator" ||
                id == "Collection" || id == "Container" || id == "Reversible")
                return t_unknown();
            if (id == "list" || id == "List" || id == "MutableSequence")
                return mk_list(t_unknown());
            if (id == "dict" || id == "Dict" || id == "Mapping" || id == "MutableMapping")
                return mk_dict(t_unknown(), t_unknown());
            if (id == "tuple" || id == "Tuple") return mk_tuple_rest(t_unknown());
            if (id == "set" || id == "Set" || id == "FrozenSet" || id == "frozenset")
                return mk_set(t_unknown());
            if (id == "object" || id == "Any") return t_unknown();
            if (auto it = classes_.find(id); it != classes_.end()) return mk_class(it->second.get());
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
            if ((base == "list" || base == "List" || base == "MutableSequence") &&
                args.size() == 1)
                return mk_list(args[0]);
            if (base == "set" || base == "Set" || base == "FrozenSet" || base == "frozenset") {
                if (args.size() == 1) return mk_set(args[0]);
            }
            if ((base == "dict" || base == "Dict" || base == "Mapping" ||
                 base == "MutableMapping") && args.size() == 2) {
                return mk_dict(args[0], args[1]);
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
            return t_unknown();
        }
        if (auto* b = dynamic_cast<BinOp*>(e); b && b->op == Tok::VBAR) {
            // int | None 这类联合类型
            auto l = parse_annotation(b->left.get(), line, depth);
            auto r = parse_annotation(b->right.get(), line, depth);
            if (l && l->kind == TyKind::None) return mk_optional(r);
            if (r && r->kind == TyKind::None) return mk_optional(l);
            if (l && l->kind == TyKind::Optional) return mk_optional(join(l->sub[0], r));
            if (r && r->kind == TyKind::Optional) return mk_optional(join(l, r->sub[0]));
            return join(l, r);
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

void TypeChecker::collect_toplevel(std::vector<StmtPtr>& body) {
        for (auto& s : body) {
            if (auto* f = dynamic_cast<FunctionDef*>(s.get())) {
                ++n_func;
                // 装饰器的返回值是装饰器决定的，静态上无从知道；
                // 把名字登记成 unknown，避免把 `@special_form def Union` 之后
                // 的 Union[...] 误报成「函数不可下标」。
                if (!f->decorators.empty()) {
                    module_->vars[f->name] = Symbol{t_unknown(), f->line, true, false};
                    continue;
                }
                auto info = build_sig(*f);
                auto t = new_ty(TyKind::Func);
                t->fn = info;
                module_->vars[f->name] = Symbol{t, f->line, true, false};
            } else if (auto* c = dynamic_cast<ClassDef*>(s.get())) {
                // 类骨架先登记：函数签名（返回标注、形参标注）可能引用
                // 类名，而签名是在 collect_toplevel 里一次性建好的。
                // 字段与方法由后面的 classdef 填进同一个对象。
                if (!classes_.count(c->name)) {
                    auto cls = std::make_shared<ClassInfo>();
                    cls->name = c->name;
                    classes_[c->name] = cls;
                    module_->vars[c->name] = Symbol{mk_class(cls.get()), c->line, true, false};
                }
            }
        }
    }

// 函数体里写 `global x` 就是在模块作用域造名字（turtledemo/colormixer.py
// 的 screen 就是 main() 里 global 出来的）。函数体按文本顺序检查，
// 早于该函数定义的其他函数读 x 时必须已经能查到，所以先整体扫一遍。
void TypeChecker::hoist_global_decls(std::vector<StmtPtr>& body) {
        for (auto& s : body) {
            switch (s->kind) {
                case StmtKind::Global:
                    for (auto& n : static_cast<Global&>(*s).names)
                        module_->vars.try_emplace(n, Symbol{t_unknown(), s->line, false, false});
                    break;
                case StmtKind::FunctionDef:
                case StmtKind::ClassDef: {
                    auto& b = s->kind == StmtKind::FunctionDef
                                  ? static_cast<FunctionDef&>(*s).body
                                  : static_cast<ClassDef&>(*s).body;
                    hoist_global_decls(b);
                    break;
                }
                case StmtKind::If: {
                    auto& f = static_cast<If&>(*s);
                    for (auto& br : f.branches) hoist_global_decls(br.body);
                    hoist_global_decls(f.orelse);
                    break;
                }
                case StmtKind::While: {
                    auto& w = static_cast<While&>(*s);
                    hoist_global_decls(w.body);
                    hoist_global_decls(w.orelse);
                    break;
                }
                case StmtKind::For: {
                    auto& f = static_cast<For&>(*s);
                    hoist_global_decls(f.body);
                    hoist_global_decls(f.orelse);
                    break;
                }
                case StmtKind::Try: {
                    auto& t = static_cast<Try&>(*s);
                    hoist_global_decls(t.body);
                    for (auto& h : t.handlers) hoist_global_decls(h.body);
                    hoist_global_decls(t.orelse);
                    hoist_global_decls(t.finalbody);
                    break;
                }
                case StmtKind::With: {
                    hoist_global_decls(static_cast<With&>(*s).body);
                    break;
                }
                default: break;
            }
        }
    }

void TypeChecker::bind_target(Expr& target, Scope* sc) {
        switch (target.kind) {
            case ExprKind::Name: {
                auto& n = static_cast<Name&>(target);
                sc->vars.try_emplace(n.id, Symbol{t_unknown(), n.line, false, false});
                return;
            }
            case ExprKind::Tuple: {
                for (auto& e : static_cast<TupleExpr&>(target).elts) bind_target(*e, sc);
                return;
            }
            case ExprKind::List: {
                for (auto& e : static_cast<ListExpr&>(target).elts) bind_target(*e, sc);
                return;
            }
            case ExprKind::Starred:
                bind_target(*static_cast<Starred&>(target).value, sc);
                return;
            default:
                return;  // a.b / a[i] 不产生新的名字
        }
    }

    void TypeChecker::hoist_module_bindings(std::vector<StmtPtr>& body) {
        hoist_scope_bindings(body, module_);
    }

    void TypeChecker::hoist_scope_bindings(std::vector<StmtPtr>& body, Scope* sc) {
        auto bind_alias = [&](const Alias& a) {
            std::string bind = a.asname.empty() ? a.name : a.asname;
            if (bind == "*") { star_import_ = true; return; }
            if (auto pos = bind.find('.'); pos != std::string::npos) bind = bind.substr(0, pos);
            sc->vars.try_emplace(bind, Symbol{t_unknown(), 1, false, false});
        };
        auto bind_target_in = [&](Expr& t) { bind_target(t, sc); };
        for (auto& s : body) {
            switch (s->kind) {
                case StmtKind::Assign: {
                    auto& a = static_cast<Assign&>(*s);
                    for (auto& t : a.targets) bind_target_in(*t);
                    break;
                }
                case StmtKind::AnnAssign:
                    bind_target_in(*static_cast<AnnAssign&>(*s).target);
                    break;
                case StmtKind::AugAssign:
                    bind_target_in(*static_cast<AugAssign&>(*s).target);
                    break;
                case StmtKind::For: {
                    auto& f = static_cast<For&>(*s);
                    bind_target_in(*f.target);
                    hoist_scope_bindings(f.body, sc);
                    hoist_scope_bindings(f.orelse, sc);
                    break;
                }
                case StmtKind::While: {
                    auto& w = static_cast<While&>(*s);
                    hoist_scope_bindings(w.body, sc);
                    hoist_scope_bindings(w.orelse, sc);
                    break;
                }
                case StmtKind::If: {
                    auto& f = static_cast<If&>(*s);
                    for (auto& b : f.branches) hoist_scope_bindings(b.body, sc);
                    hoist_scope_bindings(f.orelse, sc);
                    break;
                }
                case StmtKind::Try: {
                    auto& t = static_cast<Try&>(*s);
                    hoist_scope_bindings(t.body, sc);
                    for (auto& h : t.handlers) hoist_scope_bindings(h.body, sc);
                    hoist_scope_bindings(t.orelse, sc);
                    hoist_scope_bindings(t.finalbody, sc);
                    break;
                }
                case StmtKind::With: {
                    auto& w = static_cast<With&>(*s);
                    for (auto& item : w.items)
                        if (item.optional_vars) bind_target_in(*item.optional_vars);
                    hoist_scope_bindings(w.body, sc);
                    break;
                }
                case StmtKind::FunctionDef:
                    sc->vars.try_emplace(static_cast<FunctionDef&>(*s).name,
                                         Symbol{t_unknown(), s->line, false, false});
                    break;
                case StmtKind::ClassDef:
                    sc->vars.try_emplace(static_cast<ClassDef&>(*s).name,
                                         Symbol{t_unknown(), s->line, false, false});
                    break;
                case StmtKind::Import:
                    // import m 绑定的是模块别名，不进普通变量表；
                    // 否则 lookup_module_alias 会被一个 unknown 变量抢先命中。
                    break;
                case StmtKind::ImportFrom:
                    for (auto& al : static_cast<ImportFrom&>(*s).names) bind_alias(al);
                    break;
                default:
                    break;
            }
        }
    }

void TypeChecker::collect_fn_decls(std::vector<StmtPtr>& body,
                                   std::unordered_set<std::string>& globals,
                                   std::unordered_set<std::string>& nonlocals) {
        for (auto& s : body) {
            switch (s->kind) {
                case StmtKind::Global:
                    for (auto& n : static_cast<Global&>(*s).names) globals.insert(n);
                    break;
                case StmtKind::Nonlocal:
                    for (auto& n : static_cast<Nonlocal&>(*s).names) nonlocals.insert(n);
                    break;
                case StmtKind::If: {
                    auto& f = static_cast<If&>(*s);
                    for (auto& b : f.branches) collect_fn_decls(b.body, globals, nonlocals);
                    collect_fn_decls(f.orelse, globals, nonlocals);
                    break;
                }
                case StmtKind::While: {
                    auto& w = static_cast<While&>(*s);
                    collect_fn_decls(w.body, globals, nonlocals);
                    collect_fn_decls(w.orelse, globals, nonlocals);
                    break;
                }
                case StmtKind::For: {
                    auto& f = static_cast<For&>(*s);
                    collect_fn_decls(f.body, globals, nonlocals);
                    collect_fn_decls(f.orelse, globals, nonlocals);
                    break;
                }
                case StmtKind::Try: {
                    auto& t = static_cast<Try&>(*s);
                    collect_fn_decls(t.body, globals, nonlocals);
                    for (auto& h : t.handlers) collect_fn_decls(h.body, globals, nonlocals);
                    collect_fn_decls(t.orelse, globals, nonlocals);
                    collect_fn_decls(t.finalbody, globals, nonlocals);
                    break;
                }
                case StmtKind::With:
                    collect_fn_decls(static_cast<With&>(*s).body, globals, nonlocals);
                    break;
                case StmtKind::FunctionDef:
                case StmtKind::ClassDef:
                    // 嵌套函数 / 类是独立作用域，它们的声明不算本函数的
                    break;
                default:
                    break;
            }
        }
    }

FuncInfoPtr TypeChecker::build_sig(FunctionDef& f) {
        auto info = std::make_shared<FuncInfo>();
        info->name = f.name;
        auto& a = f.args;
        size_t total_pos = a.posonly.size() + a.args.size();
        size_t ndef = a.defaults.size();

        // 默认参数只收字面量；接口序列化需要不依赖 AST 的表示。
        auto literal_of = [](const Expr* e) -> std::optional<LiteralValue> {
            if (!e || e->kind != ExprKind::Constant) return std::nullopt;
            auto& c = static_cast<const Constant&>(*e);
            LiteralValue v;
            switch (c.ck) {
                case Constant::CK::None: v.kind = LiteralValue::Kind::None; break;
                case Constant::CK::Bool:
                    v.kind = LiteralValue::Kind::Bool;
                    v.b = c.b;
                    break;
                case Constant::CK::Int:
                    v.kind = LiteralValue::Kind::Int;
                    v.i = c.i;
                    break;
                case Constant::CK::Float:
                    v.kind = LiteralValue::Kind::Float;
                    v.f = c.f;
                    break;
                case Constant::CK::Str:
                    v.kind = LiteralValue::Kind::Str;
                    v.s = c.s;
                    break;
                default: return std::nullopt;
            }
            return v;
        };

        // 默认值只作用于「最后 ndef 个位置参数」，与位置参数前缀无关
        auto add_pos = [&](Arg& arg, bool posonly) {
            Param p;
            p.name = arg.name;
            p.ty = parse_annotation(arg.annotation.get(), f.line);
            size_t idx = info->pos.size();
            p.has_default = idx + ndef >= total_pos;
            p.kwonly = false;
            if (p.has_default) {
                size_t di = idx - (total_pos - ndef);
                if (di < a.defaults.size()) {
                    p.default_expr = a.defaults[di].get();
                    p.default_lit = literal_of(p.default_expr);
                }
            }
            info->pos.push_back(std::move(p));
            if (posonly) ++info->n_posonly;
        };
        for (auto& arg : a.posonly) add_pos(arg, true);
        for (auto& arg : a.args) add_pos(arg, false);

        for (size_t i = 0; i < a.kwonly.size(); ++i) {
            Param p;
            p.name = a.kwonly[i].name;
            p.ty = parse_annotation(a.kwonly[i].annotation.get(), f.line);
            p.has_default = i < a.kw_defaults.size() && a.kw_defaults[i] != nullptr;
            p.kwonly = true;
            if (i < a.kw_defaults.size()) {
                p.default_expr = a.kw_defaults[i].get();
                p.default_lit = literal_of(p.default_expr);
            }
            info->kwonly.push_back(std::move(p));
        }
        if (a.vararg) {
            info->varargs = true;
            info->vararg_name = a.vararg->name;
            info->vararg_ty = parse_annotation(a.vararg->annotation.get(), f.line);
        }
        if (a.kwarg) {
            info->kwargs = true;
            info->kwarg_name = a.kwarg->name;
            info->kwarg_ty = parse_annotation(a.kwarg->annotation.get(), f.line);
        }
        info->ret = parse_annotation(f.returns.get(), f.line);
        info->ret_declared = f.returns != nullptr;
        return info;
    }

// 语句块是否必然离开：最后一条是 return/raise/break/continue，
// 或者是 if/else 且所有分支都离开。用来识别提前退出的 None 守卫。
bool TypeChecker::block_exits(const std::vector<StmtPtr>& body) {
    if (body.empty()) return false;
    auto& last = *body.back();
    switch (last.kind) {
        case StmtKind::Return:
        case StmtKind::Raise:
        case StmtKind::Break:
        case StmtKind::Continue:
            return true;
        case StmtKind::If: {
            auto& f = static_cast<If&>(last);
            // 没有 else 的 if 不算离开（可能整条都不执行）
            if (f.orelse.empty()) return false;
            for (auto& b : f.branches)
                if (!block_exits(b.body)) return false;
            return block_exits(f.orelse);
        }
        default: return false;
    }
}

void TypeChecker::stmts(std::vector<StmtPtr>& body) {
    // 提前退出的 None 守卫：`if x is None: return` 之后，x 一定不是 None。
    // 收窄要保持到块末，所以在块级别统一记录、退出时恢复。
    std::vector<Narrowing> active;
    for (auto& s : body) {
        if (s->kind == StmtKind::If) {
            auto& f = static_cast<If&>(*s);
            stmt(*s);
            // 单分支且分支必然离开（return/raise/continue/break），
            // 后面的代码只可能在条件为假时到达
            if (f.orelse.empty() && f.branches.size() == 1 &&
                block_exits(f.branches[0].body)) {
                auto n = narrow_on(*f.branches[0].cond, false);
                if (n.sym) active.push_back(n);
            }
            continue;
        }
        stmt(*s);
    }
    for (auto& n : active) restore_narrowing(n);
}

void TypeChecker::stmt(Stmt& s) {
        ++n_stmt;
        switch (s.kind) {
            case StmtKind::Expr:
                expr(*static_cast<ExprStmt&>(s).value);
                break;
            case StmtKind::Assign: {
                auto& a = static_cast<Assign&>(s);
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
                for (auto& t : a.targets) assign(*t, vt, a.line, a.col);
                break;
            }
            case StmtKind::AugAssign: {
                auto& a = static_cast<AugAssign&>(s);
                auto cur_ty = expr(*a.target);
                // list += 可迭代对象：+= 走的是 list.__iadd__，吃任意 iterable，
                // 和 list + tuple 是两回事（后者才非法）
                if (a.op == Tok::PLUSEQUAL && cur_ty && cur_ty->kind == TyKind::List) {
                    auto vt = expr(*a.value);
                    auto ie = iter_elem(vt, a.line, a.col);
                    if (!assignable(cur_ty->sub[0], ie)) {
                        if (cur_ty->locked)
                            err(a.line, a.col,
                                std::format("list element type is '{}', got '{}'",
                                            type_name(cur_ty->sub[0]), type_name(ie)));
                        else
                            widen_slot(cur_ty->sub[0], cur_ty, ie);
                    }
                    break;
                }
                auto vt = expr(*a.value);
                Tok bin = aug_to_bin(a.op);
                auto rt = binop_type(bin, cur_ty, vt, a.line, a.col);
                assign(*a.target, rt, a.line, a.col);
                break;
            }
            case StmtKind::AnnAssign: {
                auto& a = static_cast<AnnAssign&>(s);
                auto ann = parse_annotation(a.annotation.get(), a.line);
                TypePtr vt;
                if (a.value) {
                    vt = expr(*a.value);
                    if (ann && !assignable(ann, vt))
                        err(a.line, a.col,
                            std::format("annotated as '{}' but assigned '{}'", type_name(ann),
                                        type_name(vt)));
                }
                if (auto* n = dynamic_cast<Name*>(a.target.get())) {
                    declare(n->id, ann ? ann : t_unknown(), a.line, a.col, true);
                } else if (auto* at = dynamic_cast<Attribute*>(a.target.get());
                           at && is_self_attr(*at)) {
                    // `self.x: int = 0`：字段标注写在方法里，等价于类体标注
                    TypePtr want = (ann && !is_unknown(ann)) ? ann : (vt ? vt : t_unknown());
                    if (!add_implicit_field(*at, want)) {
                        if (auto* f = cur_class_->field(at->attr);
                            f && !is_unknown(want) && !assignable(f->ty, want))
                            err(a.line, a.col,
                                std::format("field '{}' has type '{}', cannot annotate as '{}'",
                                            at->attr, type_name(f->ty), type_name(want)));
                    }
                }
                break;
            }
            case StmtKind::Return: {
                auto& r = static_cast<Return&>(s);
                TypePtr vt = r.value ? expr(*r.value) : t_none();
                if (!cur_fn_) {
                    err(r.line, r.col, "'return' outside function");
                    break;
                }
                if (cur_fn_->ret_declared) {
                    if (!assignable(cur_fn_->ret, vt))
                        err(r.line, r.col,
                            std::format("function '{}' is declared to return '{}', got '{}'",
                                        cur_fn_->name, type_name(cur_fn_->ret), type_name(vt)));
                } else if (rets_) {
                    rets_->push_back({vt, r.line, r.col});
                }
                break;
            }
            case StmtKind::If: {
                auto& f = static_cast<If&>(s);
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
                std::unordered_map<std::string, TypePtr> merged;
                auto merge_in = [&]() {
                    for (auto& [n, sym] : cur_->vars) {
                        auto it = merged.find(n);
                        if (it == merged.end()) merged.emplace(n, sym.ty);
                        else if (!type_eq(it->second, sym.ty)) it->second = join(it->second, sym.ty);
                    }
                };
                for (auto& b : f.branches) {
                    restore_types();
                    expr(*b.cond);
                    auto n = narrow_on(*b.cond, true);
                    stmts(b.body);
                    restore_narrowing(n);
                    merge_in();
                }
                restore_types();
                // else 分支等价于「所有条件都为假」，也要收窄：
                // `if x is None: ... else: x.foo()` 里的 x 不是 None
                Narrowing n;
                if (f.branches.size() == 1 && !f.orelse.empty())
                    n = narrow_on(*f.branches[0].cond, false);
                stmts(f.orelse);
                restore_narrowing(n);
                merge_in();
                // 写回：类型取各分支上界，分支里新出现的名字保留
                for (auto& [name, ty] : merged) {
                    auto it = cur_->vars.find(name);
                    if (it == cur_->vars.end())
                        cur_->vars.emplace(name, Symbol{ty, s.line, false, false});
                    else
                        it->second.ty = ty;
                }
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<While&>(s);
                expr(*w.cond);
                ++loop_depth_;
                stmts(w.body);
                --loop_depth_;
                stmts(w.orelse);
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<For&>(s);
                auto it = expr(*f.iter);
                auto elem = iter_elem(it, f.line, f.col);
                assign(*f.target, elem, f.line, f.col);
                ++loop_depth_;
                stmts(f.body);
                --loop_depth_;
                stmts(f.orelse);
                break;
            }
            case StmtKind::FunctionDef:
                funcdef(static_cast<FunctionDef&>(s));
                break;
            case StmtKind::ClassDef:
                classdef(static_cast<ClassDef&>(s));
                break;
            case StmtKind::Try: {
                auto& t = static_cast<Try&>(s);
                // 异常可能在任何一条语句上抛出，所以 except 分支看到的变量
                // 类型是「try 入口」与「try 结束时」的公共上界。
                // 不做这个回滚，`v = int(v)` 之后的 `except: v.replace(...)`
                // 会被当成 int 调 str 方法。
                auto snapshot = cur_->vars;
                stmts(t.body);
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
                        // py23 的异常对象就是消息字符串：as e 绑定 str
                        auto sc = std::make_unique<Scope>();
                        sc->parent = cur_;
                        cur_ = sc.get();
                        pool_.push_back(std::move(sc));
                        cur_->vars[h.name] = Symbol{t_str(), t.line, false, false};
                        stmts(h.body);
                        cur_ = cur_->parent;
                    } else {
                        stmts(h.body);
                    }
                }
                stmts(t.orelse);
                stmts(t.finalbody);
                break;
            }
            case StmtKind::With: {
                auto& w = static_cast<With&>(s);
                for (auto& item : w.items) {
                    auto t = expr(*item.context);
                    // `with Ctx() as c` 的 c 是 __enter__ 的返回值；能查到
                    // 方法签名就用它，查不到退回 unknown（with 本身不检查协议）。
                    TypePtr vt = t_unknown();
                    TypePtr ct = t;
                    if (ct && ct->kind == TyKind::Optional && !ct->sub.empty()) ct = ct->sub[0];
                    if (ct && ct->kind == TyKind::Class && ct->cls) {
                        if (auto* m = ct->cls->method("__enter__");
                            m && m->call_sig && m->call_sig->ret)
                            vt = m->call_sig->ret;
                    }
                    if (item.optional_vars)
                        assign(*item.optional_vars, vt, w.line, w.col);
                }
                stmts(w.body);
                break;
            }
            case StmtKind::Raise: {
                auto& r = static_cast<Raise&>(s);
                if (r.exc) expr(*r.exc);
                if (r.cause) expr(*r.cause);
                break;
            }
            case StmtKind::Assert: {
                auto& a = static_cast<Assert&>(s);
                expr(*a.test);
                if (a.msg) expr(*a.msg);
                break;
            }
            case StmtKind::Import: {
                auto& im = static_cast<Import&>(s);
                for (auto& al : im.names) {
                    std::string bind = al.asname.empty() ? al.name : al.asname;
                    if (auto pos = bind.find('.'); pos != std::string::npos) bind = bind.substr(0, pos);
                    if (!module_lookup_) {
                        // 单文件 check：import 没建模，保持前端宽松。
                        declare(bind, t_unknown(), s.line);
                        unsupported.push_back("import");
                        continue;
                    }
                    if (al.name.find('.') != std::string::npos && al.asname.empty()) {
                        err(s.line, s.col,
                            "dotted import needs 'as name' (or use from ... import)");
                        continue;
                    }
                    ModuleInfoPtr info = module_lookup_(al.name);
                    if (info) {
                        cur_->modules[bind] = std::move(info);
                    } else {
                        declare(bind, t_unknown(), s.line);
                        unsupported.push_back("import");
                    }
                }
                break;
            }
            case StmtKind::ImportFrom: {
                auto& im = static_cast<ImportFrom&>(s);
                if (!module_lookup_) {
                    for (auto& al : im.names) {
                        std::string bind = al.asname.empty() ? al.name : al.asname;
                        if (bind == "*") {
                            star_import_ = true;
                            continue;
                        }
                        declare(bind, t_unknown(), s.line);
                    }
                    unsupported.push_back("import");
                    break;
                }
                if (im.level != 0 || im.module.empty()) {
                    err(s.line, s.col, "relative imports are not supported yet");
                    break;
                }
                if (im.names.size() == 1 && im.names[0].name == "*") {
                    ModuleInfoPtr info = module_lookup_(im.module);
                    if (!info) {
                        star_import_ = true;
                        unsupported.push_back("import");
                        break;
                    }
                    for (auto& [n, ty] : info->globals)
                        if (!n.empty() && n[0] != '_') declare(n, ty, s.line);
                    for (auto& [n, ic] : info->classes)
                        if (!n.empty() && n[0] != '_') declare(n, mk_class(ic.cls.get()), s.line);
                    break;
                }
                ModuleInfoPtr info = module_lookup_(im.module);
                for (auto& al : im.names) {
                    std::string bind = al.asname.empty() ? al.name : al.asname;
                    if (!info) {
                        declare(bind, t_unknown(), s.line);
                        unsupported.push_back("import");
                        continue;
                    }
                    if (auto ci = info->classes.find(al.name); ci != info->classes.end()) {
                        declare(bind, mk_class(ci->second.cls.get()), s.line);
                        continue;
                    }
                    auto it = info->globals.find(al.name);
                    if (it == info->globals.end()) {
                        if (auto child = module_lookup_(im.module + "." + al.name)) {
                            cur_->modules[bind] = std::move(child);
                            continue;
                        }
                        err(s.line, s.col,
                            std::format("module '{}' has no attribute '{}'", im.module, al.name));
                        declare(bind, t_unknown(), s.line);
                        continue;
                    }
                    declare(bind, it->second, s.line);
                }
                break;
            }
            case StmtKind::Global: {
                for (auto& n : static_cast<Global&>(s).names) global_decl_.insert(n);
                break;
            }
            case StmtKind::Nonlocal: {
                auto& nl = static_cast<Nonlocal&>(s);
                for (auto& n : nl.names) {
                    nonlocal_decl_.insert(n);
                    if (cur_fn_def_) free_refs_[cur_fn_def_].insert(n);
                    bool found = false;
                    if (cur_fn_scope_) {
                        for (Scope* sc = cur_fn_scope_->parent; sc && !sc->is_module; sc = sc->parent)
                            if (sc->vars.count(n)) { found = true; break; }
                    }
                    if (!found) err(s.line, s.col, std::format("no binding for nonlocal '{}' found", n));
                }
                break;
            }
            case StmtKind::Delete: {
                for (auto& t : static_cast<Delete&>(s).targets) expr(*t);
                break;
            }
            case StmtKind::Break:
            case StmtKind::Continue:
            case StmtKind::Pass:
                break;
        }
    }

Tok TypeChecker::aug_to_bin(Tok t) {
        switch (t) {
            case Tok::PLUSEQUAL: return Tok::PLUS;
            case Tok::MINUSEQUAL: return Tok::MINUS;
            case Tok::STAREQUAL: return Tok::STAR;
            case Tok::SLASHEQUAL: return Tok::SLASH;
            case Tok::DOUBLESLASHEQUAL: return Tok::DOUBLESLASH;
            case Tok::PERCENTEQUAL: return Tok::PERCENT;
            case Tok::DOUBLESTAREQUAL: return Tok::DOUBLESTAR;
            case Tok::AMPEREQUAL: return Tok::AMPER;
            case Tok::VBAREQUAL: return Tok::VBAR;
            case Tok::CIRCUMFLEXEQUAL: return Tok::CIRCUMFLEX;
            case Tok::LSHIFTEQUAL: return Tok::LSHIFT;
            case Tok::RSHIFTEQUAL: return Tok::RSHIFT;
            default: return Tok::PLUS;
        }
    }

TypeChecker::Narrowing TypeChecker::narrow_on(Expr& cond, bool truthy) {
        auto non_none = [](const TypePtr& t) -> TypePtr {
            if (!t) return t;
            if (t->kind == TyKind::Optional)
                return t->sub.empty() ? t_unknown() : t->sub[0];
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
                Name* name = dynamic_cast<Name*>(c.left.get());
                bool none_right = is_none_const(c.comparators[0].get());
                if (!name && is_none_const(c.left.get()))
                    name = dynamic_cast<Name*>(c.comparators[0].get());
                if (name && (none_right || is_none_const(c.left.get()))) {
                    // 只做 "is not None" 方向的收窄。
                    // "is None" 分支里若把变量改成 None，`if x is None: x = {}`
                    // 这种合法写法反而会报错，收窄必须尊重原声明。
                    bool is_not = c.ops[0] == CmpOp::IsNot;
                    if (is_not != truthy) return {};
                    Symbol* sym = lookup(name->id);
                    if (!sym) return {};
                    Narrowing n{sym, sym->ty, nullptr};
                    sym->ty = non_none(sym->ty);
                    n.now = sym->ty;
                    return n;
                }
            }
        }
        if (cond.kind == ExprKind::Name && truthy) {
            auto& n = static_cast<Name&>(cond);
            if (Symbol* sym = lookup(n.id)) {
                Narrowing r{sym, sym->ty, nullptr};
                sym->ty = non_none(sym->ty);
                r.now = sym->ty;
                return r;
            }
        }
        return {};
    }

    void TypeChecker::restore_narrowing(const Narrowing& n) {
        // 块内可能已经重新赋值（类型变了），这时不该把类型恢复成收窄前的
        if (!n.sym || n.sym->ty != n.now) return;
        n.sym->ty = n.old;
    }

void TypeChecker::funcdef(FunctionDef& f) { funcdef_impl(f, nullptr); }

void TypeChecker::funcdef_impl(FunctionDef& f, const ClassInfoPtr& owner) {
        FuncInfoPtr info;
        if (owner) {
            // 方法名属于类命名空间，不进外层作用域；签名在 classdef 里
            // 已经建好（方法之间要能互相调用），这里复用同一份对象。
            auto fit = func_infos_.find(&f);
            if (fit != func_infos_.end() && fit->second) {
                info = fit->second;
            } else {
                info = build_sig(f);
                if (!info->pos.empty() && is_unknown(info->pos[0].ty))
                    info->pos[0].ty = mk_class(owner.get());
            }
        } else if (auto* sym = lookup(f.name); sym && sym->ty && sym->ty->kind == TyKind::Func &&
                                       sym->ty->fn && sym->ty->fn->name == f.name) {
            info = sym->ty->fn;
        } else {
            info = build_sig(f);
            if (f.decorators.empty()) {
                auto t = new_ty(TyKind::Func);
                t->fn = info;
                declare(f.name, t, f.line, f.col, true);
            } else {
                // 装饰器的返回值未知；函数体仍然按自己的签名检查
                declare(f.name, t_unknown(), f.line, f.col, true);
            }
        }

        if (!owner)
            if (auto* sym = lookup(f.name)) sym->from_def = true;

        // 默认值和装饰器在外层作用域求值 —— Python 是在 def 执行时算的，
        // 此时形参还不存在。先检查再进函数作用域，`def f(a, b=a)` 里的 a
        // 才不会被误解析成形参。
        for (auto& d : f.args.defaults) expr(*d);
        for (auto& d : f.args.kw_defaults)
            if (d) expr(*d);
        for (auto& dec : f.decorators) expr(*dec);

        // 装饰器的返回值常常就是包装函数：从下往上套，每一层用返回类型
        // 替换当前类型。这样调用点能按包装签名检查实参并做数值转换，
        // 而不是退化成 unknown 后按错误的调用约定传参。
        if (!owner && !f.decorators.empty()) {
            TypePtr cur = new_ty(TyKind::Func);
            cur->fn = info;
            bool known = true;
            for (auto it = f.decorators.rbegin(); it != f.decorators.rend(); ++it) {
                TypePtr dt = type_of(it->get());
                if (dt && dt->kind == TyKind::Func && dt->fn && dt->fn->ret) {
                    cur = dt->fn->ret;
                } else {
                    known = false;
                    break;
                }
            }
            if (known && cur && cur->kind == TyKind::Func) {
                if (auto* sym = lookup(f.name)) sym->ty = cur;
            }
        }

        auto prev_fn = cur_fn_;
        bool prev_yield = saw_yield_;
        auto* prev_scope = cur_;   // 必须回到进入函数前的作用域，不能一律挂到 module_
        auto sc = std::make_unique<Scope>();
        sc->parent = prev_scope;
        sc->fn_owner = &f;
        cur_ = sc.get();
        func_scopes_[&f] = sc.get();
        func_infos_[&f] = info;
        auto* prev_fn_def = cur_fn_def_;
        auto* prev_fn_scope = cur_fn_scope_;
        cur_fn_ = info;
        cur_fn_def_ = &f;
        cur_fn_scope_ = sc.get();
        loop_depth_ = 0;
        auto saved_globals = std::move(global_decl_);
        auto saved_nonlocals = std::move(nonlocal_decl_);
        global_decl_.clear();
        nonlocal_decl_.clear();
        std::vector<RetInfo> rets;
        auto* prev_rets = rets_;
        rets_ = &rets;

        for (auto& p : info->pos)
            cur_->vars[p.name] = Symbol{p.ty ? p.ty : t_unknown(), f.line, true, false};
        for (auto& p : info->kwonly)
            cur_->vars[p.name] = Symbol{p.ty ? p.ty : t_unknown(), f.line, true, false};
        if (info->varargs)
            // *args 在 Python 里是 tuple，不是 list：`(v,) + args` 合法
            cur_->vars[info->vararg_name] =
                Symbol{mk_tuple_rest(info->vararg_ty ? info->vararg_ty : t_unknown()), f.line, true,
                       false};
        if (info->kwargs)
            cur_->vars[info->kwarg_name] =
                Symbol{mk_dict(t_str(), info->kwarg_ty ? info->kwarg_ty : t_unknown()),
                       f.line, true, false};

        // global / nonlocal 作用于整个函数，先收集，再让 hoist 跳过这些名字。
        collect_fn_decls(f.body, global_decl_, nonlocal_decl_);
        hoist_scope_bindings(f.body, cur_);
        for (auto& n : global_decl_) cur_->vars.erase(n);
        for (auto& n : nonlocal_decl_) cur_->vars.erase(n);
        auto saved_self_name = cur_self_name_;  // 拷贝：下面要让嵌套函数看见它
        if (owner) cur_self_name_ = info->pos.empty() ? std::string() : info->pos[0].name;
        // 嵌套函数不改它：闭包捕获的 self 还是外层那个名字
        stmts(f.body);
        cur_self_name_ = std::move(saved_self_name);

        // 无标注函数：从所有 return 处推断返回类型
        if (!info->ret_declared) {
            TypePtr acc = nullptr;
            for (auto& r : rets) acc = acc ? join(acc, r.ty) : r.ty;
            info->ret = acc;
        }
        info->ret_known = true;

        // 生成器的返回类型无法用现有 Type 表达：调它就是拿一个可迭代对象
        if (saw_yield_) info->ret = t_unknown();
        // async def 调用得到的是协程对象，不是返回标注里的值；
        // `_c = _c()` 这类「函数名复用成结果」的写法全靠这条才不误报
        if (f.is_async) info->ret = t_unknown();

        cur_ = prev_scope;
        cur_fn_ = prev_fn;
        cur_fn_def_ = prev_fn_def;
        cur_fn_scope_ = prev_fn_scope;
        saw_yield_ = prev_yield;
        rets_ = prev_rets;
        global_decl_ = std::move(saved_globals);
        nonlocal_decl_ = std::move(saved_nonlocals);
        // 保留函数作用域：IR 降级要按名字查最终类型
        pool_.push_back(std::move(sc));
    }

bool TypeChecker::is_self_attr(const Attribute& a) const {
    if (!cur_class_ || cur_self_name_.empty()) return false;
    if (a.value->kind != ExprKind::Name) return false;
    return static_cast<const Name&>(*a.value).id == cur_self_name_;
}

// `self.x = ...` 里 x 没在类体里声明过：补成字段。Python 的实例属性本来
// 就是在 __init__ 里长出来的，强制先写标注会把标准库挡在门外。
// 返回 true 表示这次赋值已经处理完（隐式字段一律放宽，不再做类型检查）；
// 返回 false 表示字段是显式声明的，调用方仍要按标注检查类型。
bool TypeChecker::add_implicit_field(const Attribute& a, TypePtr ty) {
    if (!is_self_attr(a)) return false;
    // 只动本类声明的字段；父类的字段不能被子类的赋值悄悄改类型
    for (auto& f : cur_class_->fields) {
        if (f.name != a.attr) continue;
        if (!f.implicit) return false;
        // 任一边 unknown 就保持 unknown：不知道就是不知道，别拿后来那个
        // 具体类型当准（`self.x = make()` 之后 `self.x = None` 会把类型
        // 覆盖成 None，后面每次 self.x.anything 全被判死）
        if (is_unknown(ty) || is_unknown(f.ty)) f.ty = t_unknown();
        else f.ty = join(f.ty, ty);
        return true;
    }
    if (cur_class_->field(a.attr)) return false;   // 继承来的字段，按标注检查
    if (cur_class_->method(a.attr)) return false;  // 同名方法：报「不能给方法赋值」
    FieldInfo fi;
    fi.name = a.attr;
    // `self.x = None` 只说明「还没赋值」，拿 None 当字段类型会让后面
    // 每一次 self.x.anything 都被判死
    fi.ty = (ty && ty->kind != TyKind::None) ? ty : t_unknown();
    fi.implicit = true;
    cur_class_->fields.push_back(std::move(fi));
    return true;
}

// 类体只允许四种语句：方法定义、带类型标注的字段声明、pass、文档字符串。
// 其余（类变量赋值、if、for……）在 Python 里是「类体在独立命名空间里执行」，
// 要做对得先有类对象和动态查找，而我们的布局是编译期定死的。语法照收，
// 标记成「没建模」，由编译阶段拒绝——不能在这里报错，真实代码太常见。
void TypeChecker::classdef(ClassDef& c) {
        if (!c.decorators.empty()) {
            unsupported.push_back("class decorator");
            for (auto& d : c.decorators) expr(*d);
        }
        if (!c.keywords.empty()) {
            // metaclass=... 之类的类关键字：语法合法，语义没建模。
            // 按「解析了但还没支持」处理，不误杀真实代码。
            unsupported.push_back("class keyword arguments");
            for (auto& kw : c.keywords)
                if (kw.value) expr(*kw.value);
        }
        ClassInfo* base = nullptr;
        // 多继承没建模：布局只可能跟一个基类走，成员表必然是不全的。
        // 前端接受（标记成没建模），由后端拒绝生成——标准库里的测试类
        // 大量是 `class T(Mixin, unittest.TestCase)`，在这里报错就是误杀。
        bool open = c.bases.size() > 1;
        if (c.bases.size() > 1) {
            unsupported.push_back("multiple inheritance");
            for (auto& b : c.bases) expr(*b);
        } else if (c.bases.size() == 1) {
            auto bt = expr(*c.bases[0]);
            if (bt && bt->kind == TyKind::Class) {
                base = bt->cls;
            } else if (is_unknown(bt)) {
                // 外部导入的基类（unittest.TestCase、OrderedDict……）：我们
                // 看不到它的字段和方法，只能把整条继承链标记成「看不全」。
                open = true;
            } else if (auto* bn = dynamic_cast<Name*>(c.bases[0].get()); bn && is_type_name(bn->id)) {
                // `class A(object)`：内建类型当根类。它在内置表里是函数
                // （object() 造一个实例），不是 ClassInfo；但常见到不能报错。
                open = true;
            } else {
                err(c.bases[0]->line, c.bases[0]->col,
                    std::format("base class must be a class, got '{}'", type_name(bt)));
            }
        }

        // 复用 collect_toplevel 建的骨架（函数签名已经引用了它的指针），
        // 只有同名类重定义时才新建。
        ClassInfoPtr cls;
        if (auto it = classes_.find(c.name); it != classes_.end() && !it->second->complete) {
            cls = it->second;
            cls->fields.clear();
            cls->methods.clear();
        } else {
            cls = std::make_shared<ClassInfo>();
        }
        cls->name = c.name;
        cls->base = base;
        cls->open = open;
        if (base) {
            cls->base_fields = base->n_fields();
            cls->base_slots = base->n_slots();
            cls->has_field_defaults = base->has_field_defaults;
        }

        // 先把类名绑定好：字段标注和字符串注解可能引用类自身（`next: "Node"`）。
        // collect_toplevel 已经占了个 unknown 的坑，这里要真的换成类类型，
        // 不能走 declare —— 它会因为「已标注」而拒绝改类型。
        {
            Scope* sc = assign_target_scope(c.name);
            auto it = sc->vars.find(c.name);
            if (it != sc->vars.end()) {
                it->second.ty = mk_class(cls.get());
                it->second.annotated = true;
            } else {
                sc->vars[c.name] = Symbol{mk_class(cls.get()), c.line, true, false};
            }
        }
        auto prev = classes_.find(c.name);
        classes_[c.name] = cls;
        all_classes_.push_back(cls);
        (void)prev;  // 同名类重定义：后面的引用拿新类，旧实例仍按旧布局走
        if (!cur_->is_module) unsupported.push_back("nested class definition");

        // 字段先处理：方法体里 `self.x` 的类型要靠它。
        for (auto& s : c.body) {
            if (s->kind != StmtKind::AnnAssign) continue;
            auto& a = static_cast<AnnAssign&>(*s);
            if (a.target->kind != ExprKind::Name) {
                err(s->line, s->col, "class field must be a plain name");
                continue;
            }
            auto& fn = static_cast<Name&>(*a.target);
            TypePtr ty = parse_annotation(a.annotation.get(), s->line);
            if (is_unknown(ty)) {
                // 标注引用了解析不出来的名字（典型：类变量 mytype = int 之后
                // 写 x: mytype）。类体赋值我们不建模，这里跟着降级，别误杀。
                unsupported.push_back("unresolved class field annotation");
            }
            if (cls->field_index(fn.id) >= 0) {
                err(s->line, s->col, std::format("duplicate field '{}'", fn.id));
                continue;
            }
            bool has_default = false;
            if (a.value) {
                auto vt = expr(*a.value);
                if (!assignable(ty, vt))
                    err(a.value->line, a.value->col,
                        std::format("cannot initialize field '{}' of type '{}' with '{}'", fn.id,
                                    type_name(ty), type_name(vt)));
                has_default = true;
                cls->has_field_defaults = true;
            }
            FieldInfo fi;
            fi.name = fn.id;
            fi.ty = ty ? ty : t_unknown();
            fi.has_default = has_default;
            fi.default_expr = a.value.get();
            cls->fields.push_back(std::move(fi));
        }

        // 方法签名先全体登记，方法之间可以互相调用（包括调用本类的其他
        // 方法，以及被子类覆盖的方法名）。
        auto rebuild_call_sig = [](MethodInfo& m) {
            // 带装饰器的方法（@staticmethod / @classmethod / @property）语义
            // 由装饰器决定，静态上不猜：不给调用签名，调用时也不检查实参
            if (m.def && !m.def->decorators.empty()) {
                m.call_sig = nullptr;
                return;
            }
            if (m.sig->pos.empty()) return;
            m.call_sig = std::make_shared<FuncInfo>(*m.sig);
            m.call_sig->name = m.name;
            m.call_sig->pos.erase(m.call_sig->pos.begin());
            if (m.call_sig->n_posonly > 0) --m.call_sig->n_posonly;
        };
        int next_slot = cls->base_slots;
        for (auto& s : c.body) {
            if (s->kind != StmtKind::FunctionDef) continue;
            auto& f = static_cast<FunctionDef&>(*s);
            // 同名方法在 Python 里合法：后定义覆盖前一个（@property 的
            // getter / setter 就是同名两个 def）。两个方法体都检查，vtable
            // 里让后一个胜出；槽位沿用第一个的。
            int dup_slot = -1;
            for (auto& mm : cls->methods)
                if (mm.name == f.name) dup_slot = mm.slot;
            if (dup_slot >= 0) unsupported.push_back("duplicate method definition");
            MethodInfo m;
            m.name = f.name;
            m.sig = build_sig(f);
            if (!f.decorators.empty()) {
                // @staticmethod 的零参方法是合法的，self 检查没意义
                unsupported.push_back("method decorator");
            } else if (m.sig->pos.empty()) {
                err(f.line, f.col, std::format("method '{}' must take 'self' as first parameter",
                                               f.name));
            } else if (is_unknown(m.sig->pos[0].ty)) {
                m.sig->pos[0].ty = mk_class(cls.get());
            }
            m.def = &f;
            // 覆盖父类的方法沿用父类的槽位（vtable 前缀与父类一致），
            // 只有真正新增的方法才占新槽。
            int inherited = cls->base ? cls->base->slot_of(f.name) : -1;
            if (dup_slot >= 0) m.slot = dup_slot;
            else m.slot = inherited >= 0 ? inherited : next_slot++;
            rebuild_call_sig(m);
            // 方法体检查复用这份签名，返回类型才会回填到同一个对象上
            func_infos_[&f] = m.sig;
            cls->methods.push_back(std::move(m));
        }
        cls->own_slots = next_slot - cls->base_slots;
        cls->has_init = cls->method("__init__") != nullptr;

        // 方法体最后检查：签名、字段、self 类型都已就位。
        ClassInfo* saved_cls = cur_class_;
        cur_class_ = cls.get();
        for (auto& m : cls->methods) funcdef_impl(*m.def, cls);
        cur_class_ = saved_cls;
        // 无标注方法的返回类型是检查完才推断出来的，调用签名要重新切一份
        for (auto& m : cls->methods) rebuild_call_sig(m);
        cls->complete = true;

        // 类体里的其他语句
        for (auto& s : c.body) {
            switch (s->kind) {
                case StmtKind::FunctionDef:
                case StmtKind::AnnAssign:
                case StmtKind::Pass:
                    break;
                case StmtKind::Expr: {
                    // 文档字符串：唯一的合法表达式语句
                    auto& e = static_cast<ExprStmt&>(*s);
                    if (e.value->kind == ExprKind::Constant &&
                        static_cast<Constant&>(*e.value).ck == Constant::CK::Str)
                        break;
                    unsupported.push_back("class body statement");
                    cls->open = true;
                    break;
                }
                default:
                    // 类变量赋值、if、for…… 在 Python 里是「类体在独立命名
                    // 空间里执行」。真实代码里太常见，硬拒会误杀一大片；
                    // 按「解析了但没建模」处理，编译阶段自然会拒绝。
                    // 成员表因此不全（self.debug 这类类属性查不到），
                    // 整类标记成 open，别再连环报错。
                    unsupported.push_back("class body statement");
                    cls->open = true;
                    break;
            }
        }

    }

bool TypeChecker::target_touches_globals(Expr& target) const {
    const Expr* p = &target;
    while (p) {
        switch (p->kind) {
            case ExprKind::Subscript: p = static_cast<const Subscript*>(p)->value.get(); break;
            case ExprKind::Attribute: p = static_cast<const Attribute*>(p)->value.get(); break;
            case ExprKind::Call: {
                auto& c = *static_cast<const Call*>(p);
                auto* n = dynamic_cast<const Name*>(c.func.get());
                return n && n->id == "globals";
            }
            case ExprKind::Name: {
                auto* n = static_cast<const Name*>(p);
                return globals_aliases_.count(n->id) > 0;
            }
            default: return false;
        }
    }
    return false;
}

void TypeChecker::assign(Expr& target, TypePtr value, int line, int col) {
        // globals()[name] = ... 之后模块级名字集合就不可知了
        if ((target.kind == ExprKind::Subscript || target.kind == ExprKind::Attribute) &&
            target_touches_globals(target))
            dynamic_globals_ = true;
        if (auto* n = dynamic_cast<Name*>(&target)) {
            declare(n->id, value, line, col);
            return;
        }
        if (auto* tup = dynamic_cast<TupleExpr*>(&target)) {
            unpack_assign(tup->elts, value, line, col);
            return;
        }
        if (auto* lst = dynamic_cast<ListExpr*>(&target)) {
            unpack_assign(lst->elts, value, line, col);
            return;
        }
        if (auto* st = dynamic_cast<Starred*>(&target)) {
            assign(*st->value, value, line, col);
            return;
        }
        if (auto* sub = dynamic_cast<Subscript*>(&target)) {
            auto ot = expr(*sub->value);
            auto st = expr(*sub->slice);
            if (sub->slice->kind == ExprKind::Slice) {
                // 切片赋值在运行时才展开，类型层面只确认对象是可变的
                auto& sl = static_cast<SliceExpr&>(*sub->slice);
                if (sl.lower) expr(*sl.lower);
                if (sl.upper) expr(*sl.upper);
                if (sl.step) expr(*sl.step);
                if (!is_unknown(ot) && ot->kind != TyKind::List && ot->kind != TyKind::Optional)
                    err(line, col,
                        std::format("'{}' does not support slice assignment", type_name(ot)));
                return;
            }
            if (ot->kind == TyKind::List) {
                if (!merge_elem(ot->sub[0], ot, value, ot->locked))
                    err(line, col,
                        std::format("cannot store '{}' into '{}'", type_name(value),
                                    type_name(ot)));
            } else if (ot->kind == TyKind::Dict) {
                if (!merge_elem(ot->sub[0], ot, st, ot->locked))
                    err(line, col,
                        std::format("dict key type is '{}', got '{}'", type_name(ot->sub[0]),
                                    type_name(st)));
                if (!merge_elem(ot->sub[1], ot, value, ot->locked))
                    err(line, col,
                        std::format("cannot store '{}' into '{}'", type_name(value),
                                    type_name(ot)));
            } else if (!is_unknown(ot) && ot->kind != TyKind::Optional &&
                       ot->kind != TyKind::Class) {
                err(line, col, std::format("'{}' does not support item assignment",
                                           type_name(ot)));
            }
            return;
        }
        if (auto* at = dynamic_cast<Attribute*>(&target)) {
            if (auto* vn = dynamic_cast<Name*>(at->value.get())) {
                if (auto mod = lookup_module_alias(vn->id)) {
                    auto it = mod->globals.find(at->attr);
                    if (it == mod->globals.end()) {
                        err(line, col,
                            std::format("module '{}' has no attribute '{}'", vn->id, at->attr));
                        return;
                    }
                    if (!assignable(it->second, value))
                        err(line, col,
                            std::format("module attribute '{}.{}' has type '{}', cannot assign '{}'",
                                        vn->id, at->attr, type_name(it->second),
                                        type_name(value)));
                    return;
                }
            }
            auto ot = expr(*at->value);
            if (ot->kind == TyKind::Func) return;
            if (ot->kind == TyKind::Optional && !ot->sub.empty() &&
                ot->sub[0]->kind == TyKind::Class)
                ot = ot->sub[0];
            if (ot->kind == TyKind::Class) {
                if (!ot->cls) return;
                if (!ot->cls->complete && ot->cls != cur_class_) return;  // 骨架，无从判断
                // `self.x = ...` 且 x 没声明过：就地补成字段，类型不再苛求
                if (add_implicit_field(*at, value)) return;
                if (auto* f = ot->cls->field(at->attr)) {
                    if (!assignable(f->ty, value))
                        err(line, col,
                            std::format("field '{}' has type '{}', cannot assign '{}'", at->attr,
                                        type_name(f->ty), type_name(value)));
                    return;
                }
                if (ot->cls->method(at->attr)) {
                    // `c.__annotations__ = d` 这类：dunder 是协议，静态不管
                    if (at->attr.size() > 4 && at->attr.starts_with("__") &&
                        at->attr.ends_with("__"))
                        return;
                    // `self.x = v` 可能走 @property 的 setter，静态分不清
                    if (is_self_attr(*at)) {
                        unsupported.push_back("assignment to a property setter");
                        return;
                    }
                    err(line, col, std::format("cannot assign to method '{}'", at->attr));
                    return;
                }
                // 外部基类可能带来任意属性
                if (ot->cls->is_open()) return;
                err(line, col,
                    std::format("'{}' object has no attribute '{}'", ot->cls->name, at->attr));
                return;
            }
            if (!is_unknown(ot))
                err(line, col,
                    std::format("'{}' has no assignable attribute '{}'", type_name(ot), at->attr));
            return;
        }
        err(line, col, "cannot assign to this expression");
    }

void TypeChecker::unpack_assign(std::vector<ExprPtr>& targets, TypePtr value, int line, int col) {
        size_t n = targets.size();
        size_t star_at = n;  // n 表示没有星号目标
        for (size_t i = 0; i < n; ++i)
            if (targets[i]->kind == ExprKind::Starred) { star_at = i; break; }

        auto assign_one = [&](Expr& t, TypePtr ty) {
            if (t.kind == ExprKind::Starred) {
                assign(*static_cast<Starred&>(t).value, ty, line, col);
            } else {
                assign(t, ty, line, col);
            }
        };

        if (value->kind == TyKind::Tuple) {
            if (value->variadic) {
                // 长度不定：每个目标拿元素类型，星号目标拿 list[元素]
                TypePtr e = value->sub.empty() ? t_unknown() : value->sub[0];
                for (auto& t : targets) {
                    if (t->kind == ExprKind::Starred)
                        assign_one(*t, mk_list(e));
                    else
                        assign_one(*t, e);
                }
                return;
            }
            auto& elems = value->sub;
            if (star_at == n) {
                if (!elems.empty() && elems.size() != n) {
                    err(line, col, std::format("cannot unpack {} values into {}", elems.size(), n));
                    return;
                }
                for (size_t i = 0; i < n; ++i)
                    assign_one(*targets[i], i < elems.size() ? elems[i] : t_unknown());
                return;
            }
            // a, *b, c = (...)：星号吃中间，剩余目标与末尾元素对齐
            size_t tail = n - star_at - 1;
            if (elems.size() + 1 < n) {
                err(line, col, std::format("cannot unpack {} values into {}", elems.size(), n));
                return;
            }
            for (size_t i = 0; i < star_at; ++i)
                assign_one(*targets[i], i < elems.size() ? elems[i] : t_unknown());
            TypePtr rest = nullptr;
            for (size_t i = star_at; i + tail < elems.size(); ++i)
                rest = rest ? join(rest, elems[i]) : elems[i];
            assign_one(*targets[star_at], mk_list(rest ? rest : t_unknown()));
            for (size_t j = 0; j < tail; ++j)
                assign_one(*targets[star_at + 1 + j], elems[elems.size() - tail + j]);
            return;
        }

        if (value->kind == TyKind::List || value->kind == TyKind::Set) {
            // list[int] 只记录元素类型，不记录长度，所以逐个赋元素类型；
            // 老代码把 sub（元素类型数组）当元素列表，`[a, b] = [1, 2]` 会误报。
            TypePtr elem = value->sub.empty() ? t_unknown() : value->sub[0];
            for (auto& t : targets) {
                if (t->kind == ExprKind::Starred)
                    assign_one(*t, mk_list(elem));
                else
                    assign_one(*t, elem);
            }
            return;
        }

        if (is_unknown(value) || value->kind == TyKind::None) {
            for (auto& t : targets) assign_one(*t, t_unknown());
            return;
        }
        if (value->kind == TyKind::Class) return;  // 可能实现了 __iter__
        err(line, col, std::format("cannot unpack non-sequence '{}'", type_name(value)));
    }

TypePtr TypeChecker::expr(Expr& e) {
        TypePtr t = expr_impl(e);
        expr_types_[&e] = t;
        return t;
    }

    TypePtr TypeChecker::expr_impl(Expr& e) {
        ++n_expr;
        // 左深链（1+1+1+...）会让递归深度等于表达式长度，需要一个上限。
        // 800 是权衡出来的：ASan 构建下每层帧约 4KB，2000 层会直接打穿
        // 8MB 栈；而真实的代码里 800 层嵌套已经不存在了。
        if (expr_depth_ >= 800) {
            err(e.line, e.col, "expression nested too deeply");
            return t_unknown();
        }
        ++expr_depth_;
        struct DepthDec {
            int& d;
            ~DepthDec() { --d; }
        } depth_dec{expr_depth_};
        switch (e.kind) {
            case ExprKind::Constant: {
                auto& c = static_cast<Constant&>(e);
                switch (c.ck) {
                    case Constant::CK::None: return t_none();
                    case Constant::CK::Bool: return t_bool();
                    case Constant::CK::Int: return t_int();
                    case Constant::CK::Float: return t_float();
                    case Constant::CK::Str: return t_str();
                    case Constant::CK::Ellipsis: return t_unknown();
                }
                return t_unknown();
            }
            case ExprKind::Name: {
                auto& n = static_cast<Name&>(e);
                if (auto* sym = lookup(n.id)) return sym->ty;
                if (lookup_module_alias(n.id)) {
                    err(n.line, n.col,
                        std::format("module '{}' cannot be used as a value", n.id));
                    return t_unknown();
                }
                // 先看内置值（__name__ / __file__ / Ellipsis ...），再看可调用对象
                if (auto vt = builtin_value_type(n.id)) return vt;
                if (is_builtin_callable(n.id)) return builtin_type(n.id);
                if (is_exception_name(n.id)) return t_unknown();
                if (n.id.starts_with("__") && n.id.ends_with("__")) return t_unknown();
                // 有 from x import * 时无从知道导入了哪些名字，只能放行
                if (star_import_) return t_unknown();
                // 模块里出现过 globals()[k] = v：名字集合是动态的
                if (dynamic_globals_) return t_unknown();
                // 模块级通常是条件导入、C 扩展常量、__all__ 之外的名字，
                // 静态无从判断；函数体内才值得报
                err(n.line, n.col, std::format("undefined name '{}'", n.id));
                return t_unknown();
            }
            case ExprKind::JoinedStr: {
                // f-string 里嵌的表达式必须真的检查一遍：以前直接 return
                // t_str()，导致 `f"{undefined_name}"` 不报错，降级阶段也拿不到
                // 内嵌表达式的类型（str 被当成 int 格式化）。
                auto& j = static_cast<JoinedStr&>(e);
                for (auto& v : j.values) expr(*v);
                return t_str();
            }
            case ExprKind::FormattedValue: {
                auto& f = static_cast<FormattedValue&>(e);
                expr(*f.value);
                if (f.format_spec) expr(*f.format_spec);
                return t_str();
            }
            case ExprKind::List: {
                auto& l = static_cast<ListExpr&>(e);
                if (l.elts.empty()) return mk_list(fresh_hole());
                TypePtr acc = expr(*l.elts[0]);
                TypePtr hole = fresh_hole();
                bool all_same = true;
                for (size_t i = 1; i < l.elts.size(); ++i) {
                    auto t = expr(*l.elts[i]);
                    if (!type_eq(acc, t)) all_same = false;
                    acc = join(acc, t);
                }
                (void)all_same;
                if (is_unknown(acc)) return mk_list(hole);
                return mk_list(acc);
            }
            case ExprKind::Tuple: {
                auto& t = static_cast<TupleExpr&>(e);
                std::vector<TypePtr> elems;
                for (auto& el : t.elts) elems.push_back(expr(*el));
                return mk_tuple(std::move(elems));
            }
            case ExprKind::Set: {
                auto& s = static_cast<SetExpr&>(e);
                if (s.elts.empty()) return mk_set(fresh_hole());
                TypePtr acc = expr(*s.elts[0]);
                for (size_t i = 1; i < s.elts.size(); ++i) acc = join(acc, expr(*s.elts[i]));
                return mk_set(acc);
            }
            case ExprKind::Dict: {
                auto& d = static_cast<DictExpr&>(e);
                if (d.keys.empty()) return mk_dict(fresh_hole(), fresh_hole());
                TypePtr kt = nullptr, vt = nullptr;
                for (size_t i = 0; i < d.keys.size(); ++i) {
                    if (d.keys[i]) {
                        auto k = expr(*d.keys[i]);
                        kt = kt ? join(kt, k) : k;
                    }
                    auto v = expr(*d.values[i]);
                    vt = vt ? join(vt, v) : v;
                }
                return mk_dict(kt ? kt : t_unknown(), vt ? vt : t_unknown());
            }
            case ExprKind::BinOp: {
                auto& b = static_cast<BinOp&>(e);
                // `int | str` 这类运行时构造的联合类型：两个操作数都是类型对象，
                // 不是普通值，按值运算检查只会误报
                if (b.op == Tok::VBAR) {
                    auto type_operand = [](Expr* x) {
                        if (auto* n = dynamic_cast<Name*>(x)) return TypeChecker::is_type_name(n->id);
                        if (auto* c = dynamic_cast<Constant*>(x)) return c->ck == Constant::CK::None;
                        return false;
                    };
                    if (type_operand(b.left.get()) && type_operand(b.right.get())) return t_unknown();
                }
                auto l = expr(*b.left);
                auto r = expr(*b.right);
                return binop_type(b.op, l, r, b.line, b.col);
            }
            case ExprKind::UnaryOp: {
                auto& u = static_cast<UnaryOp&>(e);
                auto t = expr(*u.operand);
                switch (u.op) {
                    case Tok::KW_NOT: return t_bool();
                    case Tok::MINUS:
                    case Tok::PLUS:
                        // 类实例 / 绑定方法可能重载了一元运算符（@property
                        // 返回的数值最常见），静态上看不见
                        if (is_unknown(t) || is_num(t) || t->kind == TyKind::Class ||
                            t->kind == TyKind::BoundMethod)
                            return t;
                        err(u.line, u.col,
                            std::format("bad operand type for unary operator: '{}'", type_name(t)));
                        return t_unknown();
                    case Tok::TILDE:
                        if (is_unknown(t) || t->kind == TyKind::Int) return t_int();
                        err(u.line, u.col, "bad operand type for '~': expected int");
                        return t_unknown();
                    default: return t_unknown();
                }
            }
            case ExprKind::BoolOp: {
                auto& b = static_cast<BoolOp&>(e);
                TypePtr acc = nullptr;
                for (auto& v : b.values) {
                    auto t = expr(*v);
                    acc = acc ? join(acc, t) : t;
                }
                return acc ? acc : t_unknown();
            }
            case ExprKind::Compare: {
                auto& c = static_cast<Compare&>(e);
                auto l = expr(*c.left);
                for (size_t i = 0; i < c.ops.size(); ++i) {
                    auto r = expr(*c.comparators[i]);
                    compare_type(c.ops[i], l, r, c.line, c.col);
                    l = r;
                }
                return t_bool();
            }
            case ExprKind::Call:
                return call(static_cast<Call&>(e));
            case ExprKind::Subscript: {
                auto& s = static_cast<Subscript&>(e);
                // list[int] / dict[str, int]：PEP 585 的泛型别名，是类型对象不是值
                if (auto* vn = dynamic_cast<Name*>(s.value.get());
                    vn && is_type_name(vn->id)) {
                    if (!s.slice) return t_unknown();
                    if (auto* tup = dynamic_cast<TupleExpr*>(s.slice.get()))
                        for (auto& el : tup->elts) expr(*el);
                    else
                        expr(*s.slice);
                    return t_unknown();
                }
                auto ot = expr(*s.value);
                if (s.slice->kind == ExprKind::Slice) {
                    auto& sl = static_cast<SliceExpr&>(*s.slice);
                    if (sl.lower) expr(*sl.lower);
                    if (sl.upper) expr(*sl.upper);
                    if (sl.step) expr(*sl.step);
                    if (ot->kind == TyKind::List || ot->kind == TyKind::Str)
                        return ot;
                    if (ot->kind == TyKind::Tuple) {
                        // 切片长度不可知，元素类型仍是原来的元素类型：
                        // tt[-2:] 不能还是 tuple[11]，否则解包会误报
                        if (ot->variadic) return ot;
                        if (ot->sub.empty()) return mk_tuple_rest(t_unknown());
                        TypePtr acc = ot->sub[0];
                        for (size_t i = 1; i < ot->sub.size(); ++i)
                            acc = join(acc, ot->sub[i]);
                        return mk_tuple_rest(acc);
                    }
                    if (is_unknown(ot)) return t_unknown();
                    err(s.line, s.col,
                        std::format("'{}' is not sliceable", type_name(ot)));
                    return t_unknown();
                }
                auto it = expr(*s.slice);
                // 定长 tuple 的常量下标可以精确到具体元素；不做这一步，
                // `d.popitem()[0]` 这种常见写法会退化成 unknown。
                if (ot && ot->kind == TyKind::Tuple && !ot->variadic &&
                    s.slice->kind == ExprKind::Constant) {
                    auto& c = static_cast<Constant&>(*s.slice);
                    if (c.ck == Constant::CK::Int || c.ck == Constant::CK::Bool) {
                        int64_t i = c.ck == Constant::CK::Bool ? (c.b ? 1 : 0) : c.i;
                        if (i < 0) i += static_cast<int64_t>(ot->sub.size());
                        if (i >= 0 && i < static_cast<int64_t>(ot->sub.size()))
                            return ot->sub[static_cast<size_t>(i)];
                    }
                }
                return subscript_type(ot, it, s.line, s.col);
            }
            case ExprKind::Attribute: {
                auto& a = static_cast<Attribute&>(e);
                if (auto* vn = dynamic_cast<Name*>(a.value.get())) {
                    if (auto mod = lookup_module_alias(vn->id)) {
                        if (auto ci = mod->classes.find(a.attr); ci != mod->classes.end())
                            return mk_class(ci->second.cls.get());
                        auto it = mod->globals.find(a.attr);
                        if (it == mod->globals.end()) {
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
                if (is_unknown(ot)) return t_unknown();
                if (ot->kind == TyKind::Optional) {
                    // 收窄做不到逐条路径成立，但内层是类时字段类型是确定的；
                    // 拒绝访问会大面积误杀，运行时有空指针检查兜底。
                    if (ot->sub.empty() || ot->sub[0]->kind != TyKind::Class) return t_unknown();
                    ot = ot->sub[0];
                }
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
                    if (auto* f = ot->cls->field(a.attr)) return f->ty;
                    if (ot->cls->method(a.attr)) return mk_bound(ot, a.attr);
                    // `self.x` 找不到：可能是 mixin 组合出来的成员，也可能
                    // 是类体里赋的类属性——两种都不是错误，静态上认输。
                    if (is_self_attr(a)) return t_unknown();
                    // 有外部基类时成员表本来就不全，查不到不等于不存在
                    if (ot->cls->is_open()) return t_unknown();
                    err(a.line, a.col,
                        std::format("'{}' object has no attribute '{}'", ot->cls->name, a.attr));
                    return t_unknown();
                }
                // 属性（不是方法）不能当可调用对象用：int.bit_length 是方法，
                // int.numerator 是属性，`n.numerator` 取到的是数值本身
                if (is_property_name(a.attr) && has_method(ot, a.attr)) return t_unknown();
                if (has_method(ot, a.attr)) return mk_bound(ot, a.attr);
                err(a.line, a.col,
                    std::format("'{}' object has no attribute '{}'", type_name(ot), a.attr));
                return t_unknown();
            }
            case ExprKind::Slice:
                return t_unknown();
            case ExprKind::ListComp: {
                auto& c = static_cast<ListComp&>(e);
                return mk_list(comp_elem(e, c.generators, c.elt));
            }
            case ExprKind::SetComp: {
                auto& c = static_cast<SetComp&>(e);
                return mk_set(comp_elem(e, c.generators, c.elt));
            }
            case ExprKind::DictComp: {
                auto& c = static_cast<DictComp&>(e);
                enter_comp_scope(c.generators);
                auto k = expr(*c.key);
                auto v = expr(*c.value);
                exit_comp_scope();
                return mk_dict(k, v);
            }
            case ExprKind::GeneratorExp: {
                auto& c = static_cast<GeneratorExp&>(e);
                return mk_list(comp_elem(e, c.generators, c.elt));
            }
            case ExprKind::Lambda: {
                auto& l = static_cast<Lambda&>(e);
                auto info = std::make_shared<FuncInfo>();
                info->name = "<lambda>";
                for (auto& p : l.args.posonly) {
                    info->pos.push_back(Param{p.name, t_unknown(), false, false, nullptr, std::nullopt});
                    ++info->n_posonly;
                }
                for (auto& p : l.args.args)
                    info->pos.push_back(Param{p.name, t_unknown(), false, false, nullptr, std::nullopt});
                for (auto& p : l.args.kwonly)
                    info->kwonly.push_back(Param{p.name, t_unknown(), false, true, nullptr, std::nullopt});
                if (l.args.vararg) {
                    info->varargs = true;
                    info->vararg_name = l.args.vararg->name;
                    info->vararg_ty = t_unknown();
                }
                if (l.args.kwarg) {
                    info->kwargs = true;
                    info->kwarg_name = l.args.kwarg->name;
                    info->kwarg_ty = t_unknown();
                }

                auto sc = std::make_unique<Scope>();
                sc->parent = cur_;
                cur_ = sc.get();
                pool_.push_back(std::move(sc));
                for (auto& p : info->pos)
                    cur_->vars[p.name] = Symbol{p.ty, l.line, false, false};
                for (auto& p : info->kwonly)
                    cur_->vars[p.name] = Symbol{p.ty, l.line, false, false};
                if (info->varargs)
                    cur_->vars[info->vararg_name] = Symbol{mk_tuple_rest(t_unknown()), l.line,
                                                           false, false};
                if (info->kwargs)
                    cur_->vars[info->kwarg_name] = Symbol{t_unknown(), l.line, false, false};
                auto body = expr(*l.body);
                cur_ = cur_->parent;
                info->ret = body;
                info->ret_known = true;
                auto t = new_ty(TyKind::Func);
                t->fn = info;
                return t;
            }
            case ExprKind::IfExp: {
                auto& f = static_cast<IfExp&>(e);
                expr(*f.test);
                auto a = expr(*f.body);
                auto b = expr(*f.orelse);
                return join(a, b);
            }
            case ExprKind::Starred:
                return expr(*static_cast<Starred&>(e).value);
            case ExprKind::NamedExpr: {
                auto& n = static_cast<NamedExpr&>(e);
                auto vt = expr(*n.value);
                assign(*n.target, vt, n.line, n.col);
                return vt;
            }
            case ExprKind::Await:
                unsupported.push_back("await");
                return expr(*static_cast<Await&>(e).value);
            case ExprKind::Yield:
            case ExprKind::YieldFrom:
                saw_yield_ = true;
                unsupported.push_back("generator");
                return t_unknown();
            case ExprKind::Comprehension:
                return t_unknown();
        }
        return t_unknown();
    }

TypePtr TypeChecker::comp_elem(Expr& self, std::vector<ExprPtr>& gens, ExprPtr& elt) {
        (void)self;
        enter_comp_scope(gens);
        auto t = expr(*elt);
        exit_comp_scope();
        return t;
    }

void TypeChecker::enter_comp_scope(std::vector<ExprPtr>& gens) {
        auto sc = std::make_unique<Scope>();
        sc->parent = cur_;
        cur_ = sc.get();
        pool_.push_back(std::move(sc));
        for (auto& g : gens) {
            auto& c = static_cast<Comprehension&>(*g);
            auto it = expr(*c.iter);
            auto elem = iter_elem(it, c.line, c.col);
            assign(*c.target, elem, c.line, c.col);
            for (auto& cond : c.ifs) expr(*cond);
        }
    }

TypePtr TypeChecker::binop_type(Tok op, TypePtr l, TypePtr r, int line, int col) {
        if (is_unknown(l) || is_unknown(r)) return t_unknown();
        // 类实例可能重载了运算符（Fraction.__sub__、__eq__……），重载还可能
        // 来自外部基类。我们没有 dunder 协议表，静态上无从判断，放过。
        if (l->kind == TyKind::Class || r->kind == TyKind::Class ||
            l->kind == TyKind::BoundMethod || r->kind == TyKind::BoundMethod)
            return t_unknown();
        // Optional / None 需要先判空才能运算，这里不做数据流分析，直接放行
        if (l->kind == TyKind::Optional || r->kind == TyKind::Optional ||
            l->kind == TyKind::None || r->kind == TyKind::None)
            return t_unknown();
        auto bad = [&]() {
            err(line, col,
                std::format("unsupported operand type(s) for {}: '{}' and '{}'", tok_name(op),
                            type_name(l), type_name(r)));
            return t_unknown();
        };

        switch (op) {
            case Tok::PLUS:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (l->kind == TyKind::Str && r->kind == TyKind::Str) return t_str();
                if (l->kind == TyKind::List && r->kind == TyKind::List)
                    return mk_list(join(l->sub[0], r->sub[0]));
                if (l->kind == TyKind::Tuple && r->kind == TyKind::Tuple) {
                    std::vector<TypePtr> v = l->sub;
                    v.insert(v.end(), r->sub.begin(), r->sub.end());
                    return mk_tuple(std::move(v));
                }
                return bad();
            case Tok::MINUS:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (l->kind == TyKind::Set && r->kind == TyKind::Set) return l;
                return bad();
            case Tok::STAR:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (l->kind == TyKind::Str && r->kind == TyKind::Int) return t_str();
                if (l->kind == TyKind::Int && r->kind == TyKind::Str) return t_str();
                if (l->kind == TyKind::List && r->kind == TyKind::Int) return l;
                if (l->kind == TyKind::Int && r->kind == TyKind::List) return r;
                if (l->kind == TyKind::Tuple && r->kind == TyKind::Int) return l;
                return bad();
            case Tok::SLASH:
                if (is_num(l) && is_num(r)) return t_float();
                return bad();
            case Tok::DOUBLESLASH:
            case Tok::PERCENT:
                if (is_num(l) && is_num(r)) return promote(l, r);
                if (op == Tok::PERCENT && l->kind == TyKind::Str) return t_str();
                return bad();
            case Tok::DOUBLESTAR:
                if (is_num(l) && is_num(r)) return promote(l, r);
                return bad();
            case Tok::AMPER:
            case Tok::VBAR:
            case Tok::CIRCUMFLEX:
                if (l->kind == TyKind::Int && r->kind == TyKind::Int) return t_int();
                if (l->kind == TyKind::Bool && r->kind == TyKind::Bool) return t_bool();
                if (l->kind == TyKind::Set && r->kind == TyKind::Set) return l;
                if (l->kind == TyKind::Dict && r->kind == TyKind::Dict && op == Tok::VBAR)
                    return l;
                return bad();
            case Tok::LSHIFT:
            case Tok::RSHIFT:
                if (l->kind == TyKind::Int && r->kind == TyKind::Int) return t_int();
                return bad();
            default:
                return bad();
        }
    }

void TypeChecker::compare_type(CmpOp op, TypePtr l, TypePtr r, int line, int col) {
        if (is_unknown(l) || is_unknown(r)) return;
        if (l->kind == TyKind::Optional || r->kind == TyKind::Optional) return;
        switch (op) {
            case CmpOp::Eq:
            case CmpOp::Ne:
            case CmpOp::Is:
            case CmpOp::IsNot:
                return;
            case CmpOp::Lt:
            case CmpOp::Le:
            case CmpOp::Gt:
            case CmpOp::Ge:
                if (is_num(l) && is_num(r)) return;
                if (l->kind == TyKind::Str && r->kind == TyKind::Str) return;
                // 元组按字典序比较（版本号、日期这类 (a, b) 比较很常见）
                if (l->kind == TyKind::Tuple && r->kind == TyKind::Tuple) return;
                if (l->kind == TyKind::List && r->kind == TyKind::List) return;
                if (l->kind == TyKind::Set && r->kind == TyKind::Set) return;
                if (l->kind == TyKind::Class || r->kind == TyKind::Class) return;
                err(line, col,
                    std::format("'{}' and '{}' are not orderable", type_name(l), type_name(r)));
                return;
            case CmpOp::In:
            case CmpOp::NotIn:
                if (r->kind == TyKind::List || r->kind == TyKind::Set || r->kind == TyKind::Str ||
                    r->kind == TyKind::Dict || r->kind == TyKind::Tuple)
                    return;
                // 类可能实现了 __iter__（或从外部基类继承），静态上看不见
                if (r->kind == TyKind::Class) return;
                err(line, col, std::format("argument of type '{}' is not iterable", type_name(r)));
                return;
        }
    }

TypePtr TypeChecker::subscript_type(TypePtr obj, TypePtr idx, int line, int col) {
        if (is_unknown(obj) || obj->kind == TyKind::Optional) return t_unknown();
        switch (obj->kind) {
            case TyKind::List:
                if (!is_unknown(idx) && idx->kind != TyKind::Int && idx->kind != TyKind::Bool)
                    err(line, col, std::format("list index must be int, got '{}'", type_name(idx)));
                return obj->sub[0];
            case TyKind::Str:
                if (!is_unknown(idx) && idx->kind != TyKind::Int && idx->kind != TyKind::Bool)
                    err(line, col, std::format("string index must be int, got '{}'", type_name(idx)));
                return t_str();
            case TyKind::Tuple:
                // 长度不定的元组，下标拿到的就是元素类型
                return obj->variadic && !obj->sub.empty() ? obj->sub[0] : t_unknown();
            case TyKind::Dict:
                if (!assignable(obj->sub[0], idx))
                    err(line, col,
                        std::format("dict key type is '{}', got '{}'", type_name(obj->sub[0]),
                                    type_name(idx)));
                return obj->sub[1];
            default:
                // 类名下标是泛型参数（`MyList[int]`、`Generic[T]`），
                // 我们不做泛型，返回 unknown 比判死有用
                if (obj->kind == TyKind::Class) return t_unknown();
                err(line, col, std::format("'{}' object is not subscriptable", type_name(obj)));
                return t_unknown();
        }
    }

TypePtr TypeChecker::iter_elem(TypePtr it, int line, int col) {
        if (is_unknown(it) || it->kind == TyKind::Optional) return t_unknown();
        switch (it->kind) {
            case TyKind::List:
            case TyKind::Set:
                return it->sub[0];
            case TyKind::Str: return t_str();
            case TyKind::Dict: return it->sub[0];
            case TyKind::Tuple: {
                // 同构元组取元素类型；异构元组取公共上界（合不上就是 unknown）。
                // 取第一个元素会让 `for x in (1, "a")` 得到 int，属于误判。
                if (it->sub.empty()) return t_unknown();
                TypePtr acc = it->sub[0];
                for (size_t i = 1; i < it->sub.size(); ++i) acc = join(acc, it->sub[i]);
                return acc;
            }
            default:
                // 类可能实现了 __iter__（或从外部基类继承），静态上看不见
                if (it->kind == TyKind::Class) return t_unknown();
                err(line, col, std::format("'{}' object is not iterable", type_name(it)));
                return t_unknown();
        }
    }

TypePtr TypeChecker::call(Call& c) {
        // f(...) 里 f 是 Name：可能是内置函数，也可能是用户函数
        if (auto* n = dynamic_cast<Name*>(c.func.get())) {
            if (!lookup(n->id) && is_builtin_callable(n->id)) return builtin_call(n->id, c);
        }
        if (auto* a = dynamic_cast<Attribute*>(c.func.get())) {
            if (auto* vn = dynamic_cast<Name*>(a->value.get())) {
                if (auto mod = lookup_module_alias(vn->id)) {
                    if (auto ci = mod->classes.find(a->attr); ci != mod->classes.end()) {
                        auto cls = ci->second.cls;
                        if (cls->has_init && ci->second.ctor_sig) {
                            check_args(ci->second.ctor_sig, c);
                        } else if (!c.args.empty() || !c.keywords.empty()) {
                            err(c.line, c.col,
                                std::format("'{}' takes no arguments", cls->name));
                            for (auto& arg : c.args) expr(*arg);
                            for (auto& kw : c.keywords) expr(*kw.value);
                        }
                        return mk_class(cls.get());
                    }
                    auto it = mod->globals.find(a->attr);
                    if (it == mod->globals.end()) {
                        err(c.line, c.col,
                            std::format("module '{}' has no attribute '{}'", vn->id, a->attr));
                        return t_unknown();
                    }
                    auto mt = it->second;
                    if (mt && mt->kind == TyKind::Func) return check_args(mt->fn, c);
                    for (auto& arg : c.args) expr(*arg);
                    for (auto& kw : c.keywords) expr(*kw.value);
                    err(c.line, c.col, "module attribute is not callable");
                    return t_unknown();
                }
            }
            // int.bit_length / type.__new__ 这类「类型上的未绑定方法」：
            // 接收者不是值，没有可检查的方法表
            if (auto* vn = dynamic_cast<Name*>(a->value.get()); vn && is_type_name(vn->id)) {
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            auto ot = expr(*a->value);
            if (is_unknown(ot)) {
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            return method_call(ot, a->attr, c, is_self_attr(*a));
        }
        auto ft = expr(*c.func);
        if (is_unknown(ft)) {
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        if (ft->kind == TyKind::BoundMethod) return method_call(ft->sub[0], ft->name, c);
        if (ft->kind == TyKind::Class) {
            // 构造：分配 + 字段初值 + 调 __init__。参数按 __init__ 的
            // 调用签名检查（已去掉 self），返回值是实例。
            auto cls = ft->cls;
            if (!cls) {
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return t_unknown();
            }
            if (!cls->complete && cls != cur_class_) {
                // 类体还没检查到：按未知放行，别把「先使用后定义」判死
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
                return mk_class(cls);
            }
            if (cls->has_init) {
                if (auto* im = cls->method("__init__"); im && im->call_sig)
                    check_args(im->call_sig, c);
                else
                    for (auto& arg : c.args) expr(*arg);
            } else {
                // 没有 __init__ 时只允许零实参；但如果父类在外部（比如
                // unittest.TestCase），__init__ 是那边定义的，管不了。
                if ((!c.args.empty() || !c.keywords.empty()) && !cls->is_open())
                    err(c.line, c.col,
                        std::format("'{}' takes no arguments (no __init__)", cls->name));
                for (auto& arg : c.args) expr(*arg);
                for (auto& kw : c.keywords) expr(*kw.value);
            }
            return mk_class(cls);
        }
        if (ft->kind != TyKind::Func) {
            err(c.line, c.col, std::format("'{}' object is not callable", type_name(ft)));
            for (auto& arg : c.args) expr(*arg);
            for (auto& kw : c.keywords) expr(*kw.value);
            return t_unknown();
        }
        return check_args(ft->fn, c);
    }

TypePtr TypeChecker::check_args(const FuncInfoPtr& fn, Call& c) {
        // 实参和关键字实参都只求值一次。之前关键字值在循环里被求了两次，
        // 一次的类型错误会重复报两遍。
        std::vector<TypePtr> argtys;
        argtys.reserve(c.args.size());
        bool has_star = false;
        for (auto& arg : c.args) {
            if (arg->kind == ExprKind::Starred) {
                has_star = true;
                auto t = expr(*static_cast<Starred&>(*arg).value);
                argtys.push_back(iter_elem(t, arg->line, arg->col));
            } else {
                argtys.push_back(expr(*arg));
            }
        }
        std::vector<std::pair<std::string, TypePtr>> kws;
        kws.reserve(c.keywords.size());
        for (auto& kw : c.keywords) kws.emplace_back(kw.arg, expr(*kw.value));

        if (!fn || fn->unchecked) return t_unknown();

        const size_t npos = fn->pos.size();
        std::vector<bool> filled(npos, false);
        std::vector<bool> kw_filled(fn->kwonly.size(), false);
        // **mapping 调用：键在静态上不可知，缺参检查必须整体放弃
        bool has_dstar = false;
        for (auto& [name, ty] : kws)
            if (name.empty()) has_dstar = true;

        if (!has_star && argtys.size() > npos && !fn->varargs) {
            err(c.line, c.col,
                std::format("function '{}' takes {} positional argument{} but {} were given",
                            fn->name, npos, npos == 1 ? "" : "s", argtys.size()));
        }

        for (size_t i = 0; i < argtys.size(); ++i) {
            if (i < npos) {
                filled[i] = true;
                if (!assignable(fn->pos[i].ty, argtys[i]))
                    err(c.args[i]->line, c.args[i]->col,
                        std::format("argument '{}' of '{}' expects '{}', got '{}'",
                                    fn->pos[i].name, fn->name, type_name(fn->pos[i].ty),
                                    type_name(argtys[i])));
            } else if (fn->varargs && !assignable(fn->vararg_ty, argtys[i])) {
                err(c.args[i]->line, c.args[i]->col,
                    std::format("'*{}' of '{}' expects '{}', got '{}'", fn->vararg_name,
                                fn->name, type_name(fn->vararg_ty), type_name(argtys[i])));
            }
        }

        for (auto& [name, ty] : kws) {
            if (name.empty()) continue;  // **mapping：键在静态上不可知
            auto pit = std::find_if(fn->pos.begin(), fn->pos.end(),
                                    [&](const Param& p) { return p.name == name; });
            if (pit != fn->pos.end()) {
                size_t idx = static_cast<size_t>(pit - fn->pos.begin());
                if (idx < fn->n_posonly) {
                    err(c.line, c.col, std::format("'{}' is a positional-only argument of '{}'",
                                                   name, fn->name));
                    continue;
                }
                if (filled[idx]) {
                    // 同一个参数被位置实参和关键字实参同时指定（或关键字重复）
                    err(c.line, c.col,
                        std::format("'{}' got multiple values for argument '{}'", fn->name, name));
                    continue;
                }
                filled[idx] = true;
                if (!assignable(pit->ty, ty))
                    err(c.line, c.col,
                        std::format("argument '{}' of '{}' expects '{}', got '{}'", name,
                                    fn->name, type_name(pit->ty), type_name(ty)));
                continue;
            }
            auto kit = std::find_if(fn->kwonly.begin(), fn->kwonly.end(),
                                    [&](const Param& p) { return p.name == name; });
            if (kit != fn->kwonly.end()) {
                size_t idx = static_cast<size_t>(kit - fn->kwonly.begin());
                if (kw_filled[idx]) {
                    err(c.line, c.col,
                        std::format("'{}' got multiple values for argument '{}'", fn->name, name));
                    continue;
                }
                kw_filled[idx] = true;
                if (!assignable(kit->ty, ty))
                    err(c.line, c.col,
                        std::format("argument '{}' of '{}' expects '{}', got '{}'", name,
                                    fn->name, type_name(kit->ty), type_name(ty)));
                continue;
            }
            if (!fn->kwargs)
                err(c.line, c.col,
                    std::format("'{}' got an unexpected keyword argument '{}'", fn->name, name));
        }

        // 缺参检查：位置参数可以被关键字补上，关键字参数只能靠关键字
        std::string missing;
        auto add_missing = [&](const std::string& n) {
            if (!missing.empty()) missing += ", ";
            missing += "'" + n + "'";
        };
        if (!has_star && !has_dstar) {
            for (size_t i = 0; i < npos; ++i)
                if (!filled[i] && !fn->pos[i].has_default) add_missing(fn->pos[i].name);
            for (size_t i = 0; i < fn->kwonly.size(); ++i)
                if (!kw_filled[i] && !fn->kwonly[i].has_default)
                    add_missing(fn->kwonly[i].name);
        }
        if (!missing.empty())
            err(c.line, c.col,
                std::format("function '{}' missing required argument{}: {}", fn->name,
                            missing.find(',') == std::string::npos ? "" : "s", missing));

        if (fn->ret) return fn->ret;
        return fn->ret_known ? t_none() : t_unknown();
    }

bool TypeChecker::is_exception_type(const std::string& n) {
        return is_exception_name(n);
}

bool TypeChecker::is_exception_name(const std::string& n) {
        static const std::unordered_set<std::string> names = {
            "BaseException", "Exception", "ArithmeticError", "AssertionError",
            "AttributeError",  "IndexError",  "KeyError",        "LookupError",
            "MemoryError",     "NameError",   "NotImplementedError", "OSError",
            "IOError",         "OverflowError", "RecursionError", "RuntimeError",
            "StopIteration",   "SyntaxError", "TypeError",       "ValueError",
            "ZeroDivisionError", "FileNotFoundError", "PermissionError",
            "ImportError",     "ModuleNotFoundError", "UnicodeDecodeError",
            "UnicodeEncodeError", "TimeoutError", "SystemExit",   "KeyboardInterrupt",
            "Warning", "DeprecationWarning", "UserWarning", "RuntimeWarning",
            "FutureWarning", "PendingDeprecationWarning", "ResourceWarning",
            "SyntaxWarning", "BytesWarning", "ImportWarning", "UnicodeWarning",
            "EncodingWarning", "ExceptionGroup", "BaseExceptionGroup",
            "EnvironmentError", "ConnectionError", "BrokenPipeError",
            "UnicodeError", "ReferenceError", "BufferError", "EOFError",
            // 文件系统与解释器相关的异常：标准库到处在用，漏一个就是一片未定义名
            "FileExistsError", "IsADirectoryError", "NotADirectoryError",
            "IndentationError", "TabError", "SystemError", "FloatingPointError",
            "BlockingIOError", "ChildProcessError", "ConnectionAbortedError",
            "ConnectionRefusedError", "ConnectionResetError", "InterruptedError",
            "ProcessLookupError", "UnicodeTranslateError", "GeneratorExit",
            "StopAsyncIteration", "WindowsError",  // WindowsError 只在 Windows 上存在（OSError 别名）
        };
        return names.count(n) > 0;
    }

TypePtr TypeChecker::builtin_value_type(const std::string& n) {
        if (n == "__name__" || n == "__file__" || n == "__doc__") return t_str();
        if (n == "__debug__") return t_bool();
        if (n == "NotImplemented" || n == "Ellipsis") return t_unknown();
        return nullptr;
    }

bool TypeChecker::is_builtin_callable(const std::string& n) {
        static const std::unordered_set<std::string> names = {
            "print",  "len",    "range",  "abs",   "min",     "max",    "sum",
            "sorted", "reversed", "enumerate", "zip", "int",  "float",  "str",
            "bool",   "list",   "dict",   "tuple", "set",     "ord",    "chr",
            "repr",   "any",    "all",    "round", "isinstance", "input", "type",
            "map",    "filter", "divmod", "pow",   "hex",     "bin",    "oct",
            // 装饰器：语义尚未实现，但名字必须能解析，否则大量真实代码会在
            // 第一行就报未定义，淹没真正有价值的类型错误
            "staticmethod", "classmethod", "property", "abstractmethod",
            "dataclass", "override", "cached_property", "contextmanager",
            "lru_cache", "wraps", "total_ordering", "singledispatch",
            // 运行时依赖名字：类型无法静态表达，值一律为 unknown
            "open",  "getattr", "setattr", "hasattr", "delattr", "dir",   "vars",
            "id",    "hash",    "iter",    "next",    "slice",   "callable",
            "format", "super",  "exec",    "eval",    "globals", "locals",
            "compile", "__import__", "breakpoint", "help", "memoryview",
            "bytearray", "frozenset", "object", "complex", "bytes",
            "__build_class__",
            "issubclass", "ascii", "aiter", "anext",
            // site 模块注入的内置名（交互式退出），真实代码里也会用到
            "quit", "exit", "copyright", "credits", "license",
        };
        return names.count(n) > 0;
    }

TypePtr TypeChecker::builtin_type(const std::string& n) {
        auto info = std::make_shared<FuncInfo>();
        info->name = n;
        info->unchecked = true;
        // 具体规则在 builtin_call 里按名字分派，这里只给出可调用标记
        auto t = new_ty(TyKind::Func);
        t->fn = info;
        return t;
    }

TypePtr TypeChecker::builtin_call(const std::string& n, Call& c) {
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        // 关键字的值在这里求一次就够了，下面按名字分派时只查类型。
        // 再求一次会把同一个未定义名报两遍。
        std::vector<TypePtr> kwt;
        for (auto& kw : c.keywords) kwt.push_back(expr(*kw.value));

        auto need = [&](size_t lo, size_t hi) {
            if (a.size() < lo || a.size() > hi) {
                err(c.line, c.col,
                    std::format("'{}' expects {} argument{}, got {}", n,
                                lo == hi ? std::to_string(lo)
                                         : std::format("{}..{}", lo, hi),
                                hi == 1 ? "" : "s", a.size()));
                return false;
            }
            return true;
        };

        if (n == "print") {
            // print 的关键字：sep / end 参与检查，file / flush 合法但代码生成还不做，
            // 那是 build 阶段的事，前端不能替它拒——真实代码里 print(..., file=sys.stderr)
            // 到处都是，前端报错只会变成误杀。
            for (size_t i = 0; i < c.keywords.size(); ++i) {
                auto& kw = c.keywords[i];
                if (kw.arg.empty()) {
                    // print(*args, **kwargs) 是转发包装的常见写法，键名静态不可知。
                    // 放行——后端展开时自然会拒。
                    continue;
                }
                if (kw.arg != "sep" && kw.arg != "end" && kw.arg != "file" && kw.arg != "flush") {
                    err(kw.value->line, kw.value->col,
                        std::format("print() got an unexpected keyword argument '{}'", kw.arg));
                    continue;
                }
                if (kw.arg != "sep" && kw.arg != "end") continue;
                TypePtr kt = kwt[i];
                if (!is_unknown(kt) && kt->kind != TyKind::Str)
                    err(kw.value->line, kw.value->col,
                        std::format("print() '{}' must be a string, got '{}'", kw.arg,
                                    type_name(kt)));
            }
            return t_none();
        }
        if (n == "len") {
            if (a.size() != 1) return t_int();
            if (!is_unknown(a[0]) && a[0]->kind != TyKind::List && a[0]->kind != TyKind::Str &&
                a[0]->kind != TyKind::Dict && a[0]->kind != TyKind::Tuple &&
                a[0]->kind != TyKind::Set && a[0]->kind != TyKind::Class)
                err(c.line, c.col,
                    std::format("object of type '{}' has no len()", type_name(a[0])));
            return t_int();
        }
        if (n == "range") {
            for (auto& t : a)
                if (!is_unknown(t) && t->kind != TyKind::Int)
                    err(c.line, c.col,
                        std::format("'range' argument must be int, got '{}'", type_name(t)));
            return mk_list(t_int());
        }
        if (n == "abs") {
            if (a.empty()) return t_unknown();
            if (!is_unknown(a[0]) && !is_num(a[0]))
                err(c.line, c.col,
                    std::format("bad operand type for abs(): '{}'", type_name(a[0])));
            return a[0];
        }
        if (n == "min" || n == "max") {
            if (a.empty()) return t_unknown();
            if (a.size() == 1) return iter_elem(a[0], c.line, c.col);
            TypePtr acc = a[0];
            for (size_t i = 1; i < a.size(); ++i) acc = join(acc, a[i]);
            return acc;
        }
        if (n == "sum") {
            if (!a.empty()) {
                auto e = iter_elem(a[0], c.line, c.col);
                if (!is_unknown(e) && !is_num(e))
                    err(c.line, c.col,
                        std::format("sum() over non-numeric '{}'", type_name(e)));
                return is_num(e) ? e : t_int();
            }
            return t_int();
        }
        if (n == "sorted" || n == "reversed") {
            if (a.empty()) return mk_list(t_unknown());
            auto e = iter_elem(a[0], c.line, c.col);
            return mk_list(e);
        }
        if (n == "enumerate") {
            if (a.empty()) return mk_list(mk_tuple({t_int(), t_unknown()}));
            auto e = iter_elem(a[0], c.line, c.col);
            return mk_list(mk_tuple({t_int(), e}));
        }
        if (n == "zip") {
            std::vector<TypePtr> elems;
            for (auto& t : a) elems.push_back(iter_elem(t, c.line, c.col));
            return mk_list(mk_tuple(std::move(elems)));
        }
        if (n == "int") return t_int();
        if (n == "float") return t_float();
        if (n == "str") return t_str();
        if (n == "bool") return t_bool();
        if (n == "list") {
            if (a.empty()) return mk_list(fresh_hole());
            return mk_list(iter_elem(a[0], c.line, c.col));
        }
        if (n == "tuple") {
            if (a.empty()) return mk_tuple({});
            return mk_tuple_rest(iter_elem(a[0], c.line, c.col));
        }
        if (n == "set") {
            if (a.empty()) return mk_set(fresh_hole());
            return mk_set(iter_elem(a[0], c.line, c.col));
        }
        if (n == "dict") return mk_dict(t_unknown(), t_unknown());
        if (n == "ord") return t_int();
        if (n == "chr") return t_str();
        if (n == "repr") return t_str();
        if (n == "hex" || n == "bin" || n == "oct") return t_str();
        if (n == "any" || n == "all" || n == "isinstance") return t_bool();
        if (n == "round") {
            if (!a.empty() && a[0]->kind == TyKind::Float) return t_int();
            return a.empty() ? t_unknown() : a[0];
        }
        if (n == "divmod") {
            if (a.size() >= 2 && is_num(a[0]) && is_num(a[1])) {
                auto q = promote(a[0], a[1]);
                return mk_tuple({q, q});
            }
            return mk_tuple({t_unknown(), t_unknown()});
        }
        if (n == "pow") return a.size() >= 2 ? promote(a[0], a[1]) : t_unknown();
        if (n == "input") return t_str();
        if (n == "type") return t_unknown();
        if (n == "next") {
            return a.empty() ? t_unknown() : iter_elem(a[0], c.line, c.col);
        }
        if (n == "iter") {
            return a.empty() ? t_unknown() : a[0];
        }
        if (n == "open" || n == "getattr" || n == "setattr" ||
            n == "hasattr" || n == "super" || n == "vars" || n == "dir" || n == "format" ||
            n == "id" || n == "hash" || n == "callable" || n == "slice" || n == "frozenset" ||
            n == "bytearray" || n == "object" || n == "complex" || n == "memoryview") {
            unsupported.push_back(std::format("runtime builtin '{}'", n));
            // frozenset 是 set 的不可变版本，不是 list；元素类型取参数的元素
            if (n == "frozenset")
                return a.empty() ? mk_set(t_unknown()) : mk_set(iter_elem(a[0], c.line, c.col));
            if (n == "bytearray") return a.empty() ? t_unknown() : mk_list(t_unknown());
            if (n == "hasattr" || n == "callable") return t_bool();
            if (n == "format") return t_str();
            return t_unknown();
        }
        if (n == "exec" || n == "eval" || n == "globals" || n == "locals" || n == "compile" ||
            n == "__import__") {
            // 只登记，不报错：能否支持由代码生成阶段决定，类型层面只能返回 unknown
            unsupported.push_back(std::format("dynamic builtin '{}'", n));
            return t_unknown();
        }
        if (n == "map" || n == "filter") {
            if (a.size() >= 2) return mk_list(iter_elem(a[1], c.line, c.col));
            return mk_list(t_unknown());
        }
        (void)need;
        return t_unknown();
    }

bool TypeChecker::has_method(const TypePtr& t, const std::string& m) {
        if (!t) return true;
        switch (t->kind) {
            case TyKind::List: {
                static const std::unordered_set<std::string> ms = {
                    "append", "extend", "insert", "remove", "pop",    "clear",
                    "index",  "count",  "sort",   "reverse", "copy", "sort",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Str: {
                static const std::unordered_set<std::string> ms = {
                    "upper",    "lower",     "strip",  "lstrip", "rstrip", "split",
                    "rsplit",   "join",      "replace", "find",  "rfind",  "startswith",
                    "endswith", "format",    "isdigit", "isalpha", "isspace", "isupper",
                    "islower",  "title",     "capitalize", "count", "index", "zfill",
                    "ljust",    "rjust",     "center", "encode", "removeprefix",
                    "removesuffix", "splitlines", "casefold", "swapcase", "partition",
                    "rpartition",   "isnumeric", "isalnum",  "isdecimal", "isidentifier",
                    "isprintable",  "istitle",   "expandtabs", "translate", "maketrans",
                    "format_map",   "rsplit",    "ljust",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Dict: {
                static const std::unordered_set<std::string> ms = {
                    "get",   "keys",   "values", "items",  "pop",    "update",
                    "setdefault", "clear", "copy", "popitem",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Set: {
                static const std::unordered_set<std::string> ms = {
                    "add", "remove", "discard", "pop", "clear", "copy", "update",
                    "union", "intersection", "difference", "issubset", "issuperset",
                    "difference_update", "intersection_update", "symmetric_difference",
                    "symmetric_difference_update", "isdisjoint",
                };
                return ms.count(m) > 0;
            }
            case TyKind::Tuple: {
                static const std::unordered_set<std::string> ms = {"count", "index"};
                return ms.count(m) > 0;
            }
            // 数值类型没有真正的「方法表」，但这些名字确实存在；
            // 缺了就是标准库里的假错误（statistics 用 int.bit_length）。
            case TyKind::Int:
            case TyKind::Bool:
            case TyKind::Float: {
                static const std::unordered_set<std::string> ms = {
                    "bit_length", "to_bytes", "from_bytes", "as_integer_ratio",
                    "conjugate",  "hex",      "fromhex",    "is_integer",
                    "numerator",  "denominator", "real",    "imag",
                };
                return ms.count(m) > 0;
            }
            default:
                return false;
        }
    }

TypePtr TypeChecker::method_call(TypePtr recv, const std::string& name, Call& c,
                                 bool self_recv) {
        // Optional[Class] 上的方法：内层类的方法表是确定的，运行时有
        // None 检查兜底，静态上直接按内层查，避免收窄不到位就误杀。
        if (recv && recv->kind == TyKind::Optional && !recv->sub.empty() &&
            recv->sub[0]->kind == TyKind::Class)
            recv = recv->sub[0];
        std::vector<TypePtr> a;
        for (auto& arg : c.args) a.push_back(expr(*arg));
        for (auto& kw : c.keywords) expr(*kw.value);

        auto argc = [&](size_t lo, size_t hi) {
            if (a.size() < lo || a.size() > hi) {
                err(c.line, c.col,
                    std::format("'{}.{}' expects {} argument{}, got {}", type_name(recv), name,
                                lo == hi ? std::to_string(lo) : std::format("{}..{}", lo, hi),
                                hi == 1 ? "" : "s", a.size()));
            }
        };
        auto elem = [&]() { return recv->sub.empty() ? t_unknown() : recv->sub[0]; };

        if (recv->kind == TyKind::Class) {
            if (!recv->cls) return t_unknown();
            // 骨架（还没看到类体）才放宽；本类方法体里成员表是齐的
            if (!recv->cls->complete && recv->cls != cur_class_) return t_unknown();
            auto* m = recv->cls->method(name);
            if (!m) {
                // 父类是外部类型：方法可能在那边，实参也无从检查。
                // self 上调不到的方法同理——mixin 组合出来的成员静态看不见。
                if (self_recv || recv->cls->is_open()) return t_unknown();
                err(c.line, c.col,
                    std::format("'{}' object has no method '{}'", recv->cls->name, name));
                return t_unknown();
            }
            auto r = m->call_sig ? check_args(m->call_sig, c) : t_unknown();
            return r;
        }

        if (recv->kind == TyKind::List) {
            auto e = elem();
            if (name == "append") {
                argc(1, 1);
                if (a.size() == 1 && !merge_elem(recv->sub[0], recv, a[0], recv->locked))
                    err(c.line, c.col,
                        std::format("list element type is '{}', got '{}'", type_name(e),
                                    type_name(a[0])));
                return t_none();
            }
            if (name == "extend") {
                argc(1, 1);
                if (a.size() == 1) {
                    auto ie = iter_elem(a[0], c.line, c.col);
                    if (!merge_elem(recv->sub[0], recv, ie, recv->locked))
                        err(c.line, c.col,
                            std::format("list element type is '{}', got '{}'", type_name(e),
                                        type_name(ie)));
                }
                return t_none();
            }
            if (name == "insert") { argc(2, 2); return t_none(); }
            if (name == "remove") { argc(1, 1); return t_none(); }
            if (name == "pop") { argc(0, 1); return e; }
            if (name == "index") { argc(1, 3); return t_int(); }
            if (name == "count") { argc(1, 1); return t_int(); }
            if (name == "sort" || name == "reverse") { argc(0, 0); return t_none(); }
            if (name == "clear") { argc(0, 0); return t_none(); }
            if (name == "copy") { argc(0, 0); return recv; }
        }

        if (recv->kind == TyKind::Str) {
            if (name == "upper" || name == "lower" || name == "strip" || name == "lstrip" ||
                name == "rstrip" || name == "title" || name == "capitalize" ||
                name == "casefold" || name == "swapcase" || name == "removeprefix" ||
                name == "removesuffix")
                return t_str();
            if (name == "split" || name == "rsplit" || name == "splitlines")
                return mk_list(t_str());
            if (name == "join") {
                argc(1, 1);
                if (a.size() == 1) iter_elem(a[0], c.line, c.col);
                return t_str();
            }
            if (name == "replace") { argc(2, 3); return t_str(); }
            if (name == "find" || name == "rfind" || name == "count" || name == "index" ||
                name == "rindex")
                return t_int();
            if (name == "startswith" || name == "endswith")
                return t_bool();
            if (name == "isdigit" || name == "isalpha" || name == "isspace" ||
                name == "isupper" || name == "islower" || name == "isnumeric" ||
                name == "isalnum" || name == "isdecimal" || name == "isidentifier" ||
                name == "isprintable" || name == "istitle")
                return t_bool();
            if (name == "format") return t_str();
            if (name == "zfill" || name == "ljust" || name == "rjust" || name == "center" ||
                name == "expandtabs" || name == "translate" || name == "format_map")
                return t_str();
            if (name == "partition" || name == "rpartition")
                return mk_tuple({t_str(), t_str(), t_str()});
            if (name == "encode") return t_unknown();
        }

        if (recv->kind == TyKind::Dict) {
            auto kt = recv->sub.size() > 0 ? recv->sub[0] : t_unknown();
            auto vt = recv->sub.size() > 1 ? recv->sub[1] : t_unknown();
            if (name == "get") {
                argc(1, 2);
                if (!a.empty() && !assignable(kt, a[0]))
                    err(c.line, c.col,
                        std::format("dict key type is '{}', got '{}'", type_name(kt),
                                    type_name(a[0])));
                return vt;
            }
            if (name == "keys") { argc(0, 0); return mk_list(kt); }
            if (name == "values") { argc(0, 0); return mk_list(vt); }
            if (name == "items") { argc(0, 0); return mk_list(mk_tuple({kt, vt})); }
            if (name == "pop") { argc(1, 2); return vt; }
            if (name == "setdefault") { argc(1, 2); return vt; }
            if (name == "update") { argc(0, 1); return t_none(); }
            if (name == "clear") { argc(0, 0); return t_none(); }
            if (name == "copy") { argc(0, 0); return recv; }
            if (name == "popitem") { return mk_tuple({kt, vt}); }
        }

        if (recv->kind == TyKind::Tuple) {
            if (name == "count") { argc(1, 1); return t_int(); }
            if (name == "index") { argc(1, 3); return t_int(); }
        }

        if (recv->kind == TyKind::Set) {
            auto e = elem();
            if (name == "add") { argc(1, 1); return t_none(); }
            if (name == "remove" || name == "discard") { argc(1, 1); return t_none(); }
            if (name == "pop") { argc(0, 0); return e; }
            if (name == "clear") { argc(0, 0); return t_none(); }
            if (name == "copy" || name == "union" || name == "intersection" ||
                name == "difference" || name == "symmetric_difference")
                return recv;
            if (name == "update" || name == "difference_update" ||
                name == "intersection_update" || name == "symmetric_difference_update")
                return t_none();
            if (name == "issubset" || name == "issuperset" || name == "isdisjoint")
                return t_bool();
        }

        if (recv->kind == TyKind::Int || recv->kind == TyKind::Float || recv->kind == TyKind::Bool) {
            if (name == "bit_length") { argc(0, 0); return t_int(); }
            if (name == "as_integer_ratio") { argc(0, 0); return mk_tuple({t_int(), t_int()}); }
            if (name == "is_integer") { argc(0, 0); return t_bool(); }
            if (name == "to_bytes") { argc(1, 3); return t_unknown(); }
            if (name == "conjugate" || name == "real" || name == "imag" || name == "numerator" ||
                name == "denominator")
                return recv;
            // hex/fromhex/from_bytes：结果类型（str/bytes）尚未建模
            if (name == "hex" || name == "fromhex" || name == "from_bytes") return t_unknown();
        }

        err(c.line, c.col,
            std::format("'{}' object has no method '{}'", type_name(recv), name));
        return t_unknown();
    }

}  // namespace py23
