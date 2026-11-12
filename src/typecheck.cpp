// typecheck.cpp —— 类型检查驱动：作用域、声明收集、函数签名、注解解析
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
        // 单态化回填：没标注的形参在调用点拿到实参类型后，函数体是按旧
        // 的 unknown 形参查过的，需要按新签名重查一遍。只重查被回填过的
        // 函数，保持文本顺序检查的其它语义（生成器返回类型、match 分发）
        // 完全不变。
        for (int round = 0; round < 4; ++round) {
            bool changed = false;
            for (auto* fd : func_defs_) {
                auto it = func_infos_.find(fd);
                if (it == func_infos_.end() || !it->second || !it->second->sig_dirty) continue;
                it->second->sig_dirty = false;
                auto oit = func_owners_.find(fd);
                // 嵌套的合成生成器函数：重查要回到它定义时的词法父作用域，
                // 否则体里的自由变量会当成模块全局。
                Scope* saved_cur = cur_;
                if (auto nit = nested_recheck_parent_.find(fd); nit != nested_recheck_parent_.end())
                    cur_ = nit->second;
                funcdef_impl(*fd, oit == func_owners_.end() ? nullptr : oit->second);
                cur_ = saved_cur;
                changed = true;
            }
            if (!changed) break;
        }
        // 合成生成器函数的元素类型在重查后才定下来，调用点缓存的表达式
        // 类型跟着刷新——降级层按它选流式迭代路径。
        for (auto& [ce, fi] : genexpr_call_sites_)
            if (fi && fi->ret) expr_types_[ce] = fi->ret;
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
            if (id == "bytes") return t_bytes();
            if (id == "file") return t_file();
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
            if ((base == "Future" || base == "Task" || base == "Coroutine" ||
                 base == "Generator" || base == "AsyncGenerator") &&
                !args.empty()) {
                TypePtr inner = args[0];
                if (base == "Future") return mk_future(inner);
                if (base == "Task") return mk_task(inner);
                if (base == "Coroutine") return mk_coroutine(inner);
                if (base == "Generator") return mk_generator(inner);
                return mk_async_generator(inner);
            }
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
        if (auto* at = dynamic_cast<Attribute*>(e)) {
            if (auto* vn = dynamic_cast<Name*>(at->value.get())) {
                if (auto mod = lookup_module_alias(vn->id); mod && mod->builtin) {
                    if (mod->name == "random" && at->attr == "Random") return t_random();
                    if (mod->name == "subprocess" && at->attr == "CompletedProcess")
                        return t_completed();
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
        info->is_async = f.is_async;
        if (f.is_async) {
            // 调用 async def 得到协程对象；return/await 看到的是结果类型。
            info->async_result = info->ret;
            info->ret = mk_coroutine(info->ret ? info->ret : t_unknown());
        }
        return info;
    }

// 语句块是否必然离开：最后一条是 return/raise/break/continue，
// 或者是 if/else 且所有分支都离开。用来识别提前退出的 None 守卫。

}  // namespace py23
