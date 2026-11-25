// typecheck/typecheck.cpp —— 类型检查驱动：作用域、声明收集、函数签名、注解解析
#include "driver/build.h"
#include "typecheck/typecheck.h"

#include <deque>

#include "ir/module.h"

namespace pyadamas {

// 所有权契约的运行时校验：TypeChecker 析构后，它拥有的 ClassInfo/
// StructInfo 全部标死。Type::cls / Type::st 是裸指针，若在此时被访问，
// 说明有 TypePtr 活过了它的 owner —— 那是 use-after-free。
TypeChecker::~TypeChecker() {
    for (auto& c : all_classes_)
        if (c) c->alive = false;
    for (auto& st : all_structs_)
        if (st) st->alive = false;
}

void TypeChecker::check_once(Module& m) {
        auto mod = std::make_unique<Scope>();
        mod->is_module = true;
        module_ = mod.get();
        cur_ = module_;
        const char* ts = std::getenv("PYADAMAS_TYPE_STATS");
        type_stats_on_ = ts && std::string_view(ts) != "0";
        pool_.push_back(std::move(mod));

        // 签名在 collect_toplevel 里一次建好，早于 import 语句的正式执行；
        // 形参/返回注解里的 `mod.Cls`、`from mod import Cls` 因此只能解析
        // 成 unknown，一路带到降级层报「attribute access is only supported
        // as a method call」。先把顶层导入的绑定登记进模块作用域，签名构建
        // 期就能看到这些类。正式 stmt_import 稍后照常执行（重复登记幂等）。
        hoist_import_bindings(m.body);
        collect_toplevel(m.body);
        hoist_module_bindings(m.body);
        hoist_global_decls(m.body);
        for (auto& s : m.body) stmt(*s);
        // 单态化回填：没标注的形参在调用点拿到实参类型后，函数体是按旧
        // 的 unknown 形参查过的，需要按新签名重查一遍。只重查被回填过的
        // 函数，保持文本顺序检查的其它语义（生成器返回类型、match 分发）
        // 完全不变。
        // 单态化回填的定点迭代。没标注的形参在调用点拿到实参类型后，函数体
        // 是按旧的 unknown 形参查过的，需要按新签名重查。
        //
        // 这里用 worklist 而不是「固定 N 轮」：每重查一个函数，它可能让
        // 别的函数变 dirty（互递归、A 调 B 调 A），固定轮数会在依赖链
        // 深度超过 N 时静默截断，留下半收敛的签名。worklist 一直跑到
        // 没有 dirty 为止；max_iterations 只是防「收敛不了」的死循环兜底
        // （正常代码远用不到）。
        {
            std::deque<FunctionDef*> worklist(func_defs_.begin(), func_defs_.end());
            std::unordered_set<FunctionDef*> queued(func_defs_.begin(), func_defs_.end());
            size_t max_iterations = func_defs_.size() * 8 + 64;
            while (!worklist.empty() && max_iterations-- > 0) {
                FunctionDef* fd = worklist.front();
                worklist.pop_front();
                queued.erase(fd);
                auto it = func_infos_.find(fd);
                if (it == func_infos_.end() || !it->second || !it->second->sig_dirty) continue;
                it->second->sig_dirty = false;
                auto oit = func_owners_.find(fd);
                // 嵌套的合成生成器函数：重查要回到它定义时的词法父作用域，
                // 否则体里的自由变量会当成模块全局。
                Scope* saved_cur = cur_;
                if (auto nit = nested_recheck_parent_.find(fd);
                    nit != nested_recheck_parent_.end())
                    cur_ = nit->second;
                auto owner = oit == func_owners_.end() ? nullptr : oit->second;
                auto* saved_class = cur_class_;
                cur_class_ = owner.get();
                funcdef_impl(*fd, owner);
                cur_class_ = saved_class;
                cur_ = saved_cur;
                if (owner) {
                    for (auto& method : owner->methods) {
                        if (method.def != fd || !method.call_sig || method.decorated) continue;
                        method.call_sig->ret = method.sig->ret;
                        method.call_sig->ret_known = method.sig->ret_known;
                        for (size_t i = 0; i < method.call_sig->pos.size(); ++i)
                            method.call_sig->pos[i] = method.sig->pos[i + method.call_sig->outer_off];
                        method.call_sig->kwonly = method.sig->kwonly;
                    }
                }
                // 重查可能把别的函数标脏（形参回填 / 装饰器 / 互递归）：
                // 把所有新脏的重新入队。没有 call graph，按全表扫一遍，
                // 只对 dirty 且未入队的入队——摊还下来仍是收敛级。
                for (auto* other : func_defs_) {
                    if (queued.count(other)) continue;
                    auto oit2 = func_infos_.find(other);
                    if (oit2 == func_infos_.end() || !oit2->second || !oit2->second->sig_dirty)
                        continue;
                    worklist.push_back(other);
                    queued.insert(other);
                }
            }
        }
        // 装饰器函数体可能在上面的单态化回填里重查过（第一个形参被回填
        // 成被装饰方法的类型），返回的函数类型会变精确。装饰方法的
        // decorated_ty / call_sig 和类属性类型在这里按最终类型刷新一次。
        for (auto& ci : all_classes_) {
            if (!ci) continue;
            for (auto& mm : ci->methods) {
                if (!mm.decorated || !mm.sig || !mm.def || mm.def->decorators.empty()) continue;
                TypePtr cur = new_ty(TyKind::Func);
                cur->fn = mm.sig;
                bool known = true;
                for (auto it = mm.def->decorators.rbegin();
                     it != mm.def->decorators.rend(); ++it) {
                    TypePtr dt = type_of(it->get());
                    if (dt && dt->kind == TyKind::Func && dt->fn && dt->fn->ret)
                        cur = dt->fn->ret;
                    else {
                        known = false;
                        break;
                    }
                }
                mm.decorated_ty = known ? cur : t_unknown();
                // 第一遍检查时装饰器返回类型可能还没定下来，类属性表里
                // 没有这个方法。调用点要靠类属性里的函数值做间接调用，
                // 无论最终类型是否已知都补一条。
                ClassAttrInfo* attr = nullptr;
                for (auto& ca : ci->class_attrs)
                    if (ca.name == mm.name) {
                        attr = &ca;
                        break;
                    }
                if (!attr) {
                    ClassAttrInfo ai;
                    ai.name = mm.name;
                    ai.ty = t_unknown();
                    ai.owner = ci.get();
                    ci->class_attrs.push_back(std::move(ai));
                    attr = &ci->class_attrs.back();
                }
                if (known && cur && cur->kind == TyKind::Func) attr->ty = cur;
                if (!known || !cur || cur->kind != TyKind::Func || !cur->fn) {
                    mm.call_sig = nullptr;
                    continue;
                }
                mm.call_sig = std::make_shared<FuncInfo>(*cur->fn);
                mm.call_sig->name = mm.name;
                mm.call_sig->outer = mm.sig.get();
                mm.call_sig->outer_off = 0;
                if (mm.call_sig->pos.empty()) {
                    mm.call_sig = nullptr;
                    continue;
                }
                mm.call_sig->pos.erase(mm.call_sig->pos.begin());
                mm.call_sig->outer_off = 1;
                if (mm.call_sig->n_posonly > 0) --mm.call_sig->n_posonly;
            }
        }
        // 合成生成器函数的元素类型在重查后才定下来，调用点缓存的表达式
        // 类型跟着刷新——降级层按它选流式迭代路径。
        for (auto& [ce, fi] : genexpr_call_sites_)
            if (fi && fi->ret) expr_types_[ce] = fi->ret;

        // 函数体都查完了，dunder 的返回类型此时才有定论。
        check_inferred_default_overrides();
        check_dunder_returns();
        // u64 大字面量收尾：没被 u64 上下文消费的照旧报 too large for int64。
        check_u64_literals();
        // 模块级作用域里没填上的洞归一化成全局 unknown 单例（见 helper 注释）。
        for (auto& [n, sym] : module_->vars) normalize_type_holes(sym.ty, 0);
        if (type_stats_on_) {
            // import 别名（from x import y）单独归一类。
            std::set<std::string> import_aliases;
            for (auto& s : m.body) {
                if (s->kind == StmtKind::Import) {
                    for (auto& al : static_cast<Import&>(*s).names) {
                        std::string bind = al.asname.empty() ? al.name : al.asname;
                        if (auto pos = bind.find('.'); pos != std::string::npos)
                            bind = bind.substr(0, pos);
                        if (bind != "*") import_aliases.insert(bind);
                    }
                    continue;
                }
                if (s->kind != StmtKind::ImportFrom) continue;
                auto& imf = static_cast<ImportFrom&>(*s);
                for (auto& al : imf.names) {
                    std::string bind = al.asname.empty() ? al.name : al.asname;
                    if (auto pos = bind.find('.'); pos != std::string::npos) bind = bind.substr(0, pos);
                    if (bind != "*") import_aliases.insert(bind);
                }
            }
            for (auto& [n, sym] : module_->vars) {
                if (sym.ty && (sym.ty->kind == TyKind::Func || sym.ty->kind == TyKind::Class)) continue;
                if (is_unknown(sym.ty) && !sym.annotated) {
                    if (import_aliases.count(n))
                        type_stat("import_alias", n);
                    else
                        type_stat("global_unknown", n);
                } else if (type_contains_unknown_elem(sym.ty)) {
                    type_stat("container_elem", n);
                }
            }
            for (auto& [cat, cnt] : type_stats_) {
                fprintf(stderr, "[type-stats] cat=%s count=%ld examples=", cat.c_str(), cnt);
                auto& ex = type_stats_examples_[cat];
                for (size_t i = 0; i < ex.size(); ++i)
                    fprintf(stderr, "%s%s", i ? "," : "", ex[i].c_str());
                fprintf(stderr, "\n");
            }
        }
    }

void TypeChecker::recheck_synth_gen(const FuncInfoPtr& fn) {
    if (!fn || !fn->sig_dirty) return;
    for (auto* fd : func_defs_) {
        auto it = func_infos_.find(fd);
        if (it == func_infos_.end() || it->second != fn) continue;
        fn->sig_dirty = false;
        Scope* saved_cur = cur_;
        if (auto nit = nested_recheck_parent_.find(fd); nit != nested_recheck_parent_.end())
            cur_ = nit->second;
        auto oit = func_owners_.find(fd);
        auto owner = oit == func_owners_.end() ? nullptr : oit->second;
        auto* saved_class = cur_class_;
        cur_class_ = owner.get();
        funcdef_impl(*fd, owner);
        cur_class_ = saved_class;
        cur_ = saved_cur;
        return;
    }
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
        deleted_names_.erase(name);  // 0.537：重新赋值即复活（保守版 del）
        // `x = None` 只是占位或清空，不是「这个槽位就是 None」。
        // 让它参与收窄会把 `loader_cls = None` 之前推出来的类型抹掉，
        // 也会把「先置 None、分支里再赋值」的代码判成 None 属性访问。
        bool assigning_none = ty && ty->kind == TyKind::None;
        Scope* sc = assign_target_scope(name);
        // 函数作用域内一个名字只允许一种标注类型，**块内的声明也算**：
        // Python 的名字是函数作用域，`if ...: c: str = ...` 与后面函数级的
        // `c: big = ...` 指的是同一个槽。只按当前作用域查 vars 会漏掉已经
        // 结束的块（块作用域是临时的），于是同一个名字被解释成两种类型，
        // 后续按旧类型检查 → `c == 0` 恒假这类静默错型。
        if (annotated && cur_fn_scope_ && ty && !assigning_none) {
            auto& seen = cur_fn_scope_->ann_seen;
            auto sit = seen.find(name);
            if (sit == seen.end()) {
                seen[name] = {ty, line};
            } else if (!assignable(sit->second.first, ty)) {
                err(line, col,
                    std::format("cannot assign '{}' to '{}' variable '{}' "
                                "(annotated at line {})",
                                type_name(ty), type_name(sit->second.first), name,
                                sit->second.second));
                return;
            }
        }
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
            sym.dynamic = false;  // 显式标注优先于此前的动态退化
            return;
        }
        // 槽已被 for 的异构绑定退化成动态表示（见 stmt_for）：后续赋值
        // 不能再把槽收窄回具体类型，否则前面循环的存储点会按新类型做
        // 逐元素转换，而读取点仍按当时的元素类型解释（位模式错读）。
        if (sym.dynamic) return;
        if (sym.annotated) {
            // 函数名被重新绑定成非函数的东西（协程对象、装饰器返回值）：
            // 继续按原签名检查只会误报，直接退化成 unknown
            if (sym.from_def && (!is_unknown(ty) && ty->kind != TyKind::Func)) {
                sym.ty = t_unknown();
                return;
            }
            // 给有标注的变量赋 None 当「清空」用，这是 Python 里的常见写法，
            // 真当成类型错误会大面积误杀；保持原类型，不收窄也不报错。
            // 空容器字面量重赋值（cur = {}）：洞按已有标注填。
            if (ty && ty_contains_hole(ty, 0)) fill_holes_recursive(ty, sym.ty, 0);
            // 0.498（窄化配套）：给 `T | None` 标注的变量赋一个确定值，槽里
            // 从此就是那个确定类型 —— 否则 `if x is None: x = 0` 之后分支合并
            // 把 Optional 又并回来，下一步 `x + 1` 误报 may be None。
            // 只对 Optional 生效：float 等其它标注的槽语义（裸 double vs 整数）
            // 不能因为这个改动漂移。
            if (!assigning_none && ty && sym.ty && sym.ty->kind == TyKind::Optional &&
                assignable(sym.ty, ty)) {
                sym.ty = ty;
                return;
            }
            if (!assigning_none && !assignable(sym.ty, ty))
                err(line, col,
                    std::format("cannot assign '{}' to '{}' variable '{}'", type_name(ty),
                                type_name(sym.ty), name));
            return;
        }
        if (assigning_none) return;  // 保持已有类型，不清空也不收窄
        // 模块级全局的「先定义后初始化」：_P = None if cond else 92 这类条件
        // 赋值会把类型并成 T | None，随后 a < _P 就被判成「可能为 None」而拒绝
        // （multiprocessing/util.py 的 _SUN_PATH_MAX）。模块级名字的值取决于模块
        // 代码跑到哪一步，已经有一处确定类型时不要把 None 并回来 —— 与上面
        // 「模块级 None 占位 → unknown」同一条口径。只对未标注的模块级名字生效：
        // x: int | None = ... 是显式语义，必须保留。
        if (sc->is_module && !sym.annotated && ty && ty->kind == TyKind::Optional &&
            ty->celem == CKind::None && ty->cdims.empty() && !ty->st && !assigning_none &&
            sym.ty && !is_unknown(sym.ty) && sym.ty->kind != TyKind::Optional &&
            sym.ty->kind != TyKind::None && !ty->sub.empty())
            ty = ty->sub[0];
        // 无标注变量按 Python 习惯可以改类型：x = None 只是占位，
        // x = [] 的元素类型待定。取公共上界；合不上就退化成 unknown，
        // 后续检查自动放行 —— 宁可漏报，不可误杀。
        if (sym.ty && sym.ty->kind == TyKind::None) { sym.ty = ty; return; }
        if (is_unknown(sym.ty)) { sym.ty = ty; return; }
        // 空容器字面量重赋值：洞按已推出的类型填（否则建表种类退化）。
        if (ty && ty_contains_hole(ty, 0)) fill_holes_recursive(ty, sym.ty, 0);
        // **float 与「非数值」互串要说清楚**：float 在槽里是裸 double，
        // 而 str/list/dict 是堆指针。同一个未标注变量先存 float 再存堆值（或
        // 反过来），槽要么按 double 读指针、要么按指针 dec double —— 实测打印
        // 位模式、甚至段错误。int/float/big 之间互串没问题（会按槽类型转换），
        // 这里只拦「一边 float、另一边不是数值」的情况，宁可报错也不静默算错。
        if (sym.ty && ty) {
            auto num = [](const TypePtr& t) {
                return t->kind == TyKind::Int || t->kind == TyKind::Bool ||
                       t->kind == TyKind::Float || t->kind == TyKind::Big ||
                       t->kind == TyKind::I8 || t->kind == TyKind::I16 ||
                       t->kind == TyKind::I32 || t->kind == TyKind::U8 ||
                       t->kind == TyKind::U16 || t->kind == TyKind::U32 ||
                       t->kind == TyKind::U64;
            };
            bool float_mix = (sym.ty->kind == TyKind::Float) != (ty->kind == TyKind::Float);
            if (float_mix && !(num(sym.ty) && num(ty))) {
                err(line, col,
                    std::format("variable '{}' holds '{}', cannot rebind to '{}' "
                                "(a slot cannot hold both a float and a non-numeric value; "
                                "annotate the variable if the types are intentional)",
                                name, type_name(sym.ty), type_name(ty)));
                return;
            }
        }
        if (assignable(sym.ty, ty)) {
            // 来源 unknown 时不要死守旧类型：变量可能已经换了一种东西
            if (is_unknown(ty)) sym.ty = t_unknown();
            return;
        }
        sym.ty = join(sym.ty, ty);
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
                signature_defs_[info.get()] = f;
                auto t = new_ty(TyKind::Func);
                t->fn = info;
                module_->vars[f->name] = Symbol{t, f->line, true, false};
            } else if (auto* c = dynamic_cast<ClassDef*>(s.get())) {
                // 类骨架先登记：函数签名（返回标注、形参标注）可能引用
                // 类名，而签名是在 collect_toplevel 里一次性建好的。
                // 字段与方法由后面的 classdef 填进同一个对象。
                class_obj_names_.insert(c->name);
                if (!classes_.count(c->name)) {
                    auto cls = std::make_shared<ClassInfo>();
                    cls->name = c->name;
                    classes_[c->name] = cls;
                    module_->vars[c->name] = Symbol{mk_class(cls.get()), c->line, true, false};
                }
            } else if (auto* st = dynamic_cast<StructDef*>(s.get())) {
                // struct 的字段只有 int/float/bool，布局不依赖其它声明，
                // 这里一次算完，函数签名里就能用 `p: Point`。
                build_struct(*st);
            }
        }
    }

// 函数体里写 `global x` 就是在模块作用域造名字（turtledemo/colormixer.py
// 的 screen 就是 main() 里 global 出来的）。函数体按文本顺序检查，
// 早于该函数定义的其他函数读 x 时必须已经能查到，所以先整体扫一遍。
// 归一化未填的「类型洞」：函数/模块类型检查完成后，作用域里仍是 fresh_hole
// 的符号类型要变回全局 unknown 单例。
//
// 洞只是**推断期**的待填占位；剩下没填上的洞如果留到 lowering，会被当成
// 「已知的 Unknown 类型」——codegen 的 unknown 动态分派只认 t_unknown() 单例
// （stdlib/hmac.py 的 `h = hashlib.md5(...)` 两分支各赋值一次，merge 后停在
// 洞上，`h.digest()` 就报「method not supported」）。
void TypeChecker::type_stat(const std::string& cat, const std::string& example) {
    if (!type_stats_on_) return;
    type_stats_[cat] += 1;
    auto& v = type_stats_examples_[cat];
    if (v.size() < 6) v.push_back(example);
}

bool type_contains_unknown_elem(const TypePtr& t) {
    if (!t || t->kind == TyKind::Unknown) return false;
    switch (t->kind) {
        case TyKind::List:
        case TyKind::Set:
        case TyKind::Tuple:
        case TyKind::Generator:
        case TyKind::Coroutine:
        case TyKind::AsyncGenerator:
        case TyKind::Dict:
            break;
        default:
            return false;
    }
    for (auto& sub : t->sub)
        if (is_unknown(sub) || type_contains_unknown_elem(sub)) return true;
    return false;
}

void normalize_type_holes(TypePtr& t, int depth) {
    if (!t || depth > 6) return;
    if (t->kind == TyKind::Unknown) {
        if (t.get() != t_unknown().get()) t = t_unknown();
        return;
    }
    for (auto& sub : t->sub) normalize_type_holes(sub, depth + 1);
}

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
                // 分级占位：只有**模块级**名字用 fresh_hole（可被后续赋值/分支合并
                // 填成已知类型：sieve 的 q = p*p 只出现在 if 分支里，占位是
                // t_unknown() 时 join 永远停在 unknown，循环里每轮多一次 RC
                // 调用。函数局部名保持 t_unknown()：局部类型推断依赖「重复赋值
                // 就换类型」的旧语义（stdlib/hmac.py 的 h = md5(...) / h = sha1(...)
                // 多分支各赋值一次，洞会让旧路径退化成 unknown 报 method 不支持）。
                sc->vars.try_emplace(n.id, Symbol{sc->is_module ? fresh_hole() : t_unknown(),
                                                  n.line, false, false});
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

    // 顶层 import / from ... import 的绑定预登记：签名构建（collect_toplevel）
    // 早于正式导入执行，注解里的模块名/导入类必须在那之前就能解析。
    // 只登记「确实可见」的模块与名字，诊断与 unsupported 记录仍由正式的
    // stmt_import 负责；cimport 有加载 .cpya 的副作用，不在这里预跑。
    void TypeChecker::hoist_import_bindings(std::vector<StmtPtr>& body) {
        if (!module_lookup_) return;
        for (auto& s : body) {
            if (auto* im = dynamic_cast<Import*>(s.get())) {
                for (auto& al : im->names) {
                    if (al.name.find('.') != std::string::npos && al.asname.empty()) continue;
                    std::string bind = al.asname.empty() ? al.name : al.asname;
                    if (auto pos = bind.find('.'); pos != std::string::npos)
                        bind = bind.substr(0, pos);
                    if (auto info = module_lookup_(al.name))
                        module_->modules[bind] = std::move(info);
                }
                continue;
            }
            if (auto* im = dynamic_cast<ImportFrom*>(s.get())) {
                if (im->level != 0 || im->module.empty()) continue;
                auto info = module_lookup_(im->module);
                if (!info || info->cpy_proxy) continue;
                if (im->names.size() == 1 && im->names[0].name == "*") {
                    for (auto& [n, ty] : info->globals)
                        if (!n.empty() && n[0] != '_')
                            module_->vars.try_emplace(n, Symbol{ty, im->line, false, false});
                    for (auto& [n, ic] : info->classes) {
                        if (n.empty() || n[0] == '_') continue;
                        module_->vars[n] =
                            Symbol{mk_class(ic.cls.get()), im->line, false, false};
                        class_obj_names_.insert(n);
                    }
                    continue;
                }
                for (auto& al : im->names) {
                    std::string bind = al.asname.empty() ? al.name : al.asname;
                    if (auto ci = info->classes.find(al.name); ci != info->classes.end()) {
                        module_->vars[bind] =
                            Symbol{mk_class(ci->second.cls.get()), im->line, false, false};
                        class_obj_names_.insert(bind);
                        continue;
                    }
                    if (auto it = info->globals.find(al.name); it != info->globals.end()) {
                        module_->vars[bind] = Symbol{it->second, im->line, false, false};
                        continue;
                    }
                    if (auto child = module_lookup_(im->module + "." + al.name))
                        module_->modules[bind] = std::move(child);
                }
            }
        }
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
                case StmtKind::ImportFrom: {
                    auto& imf = static_cast<ImportFrom&>(*s);
                    // from python import m 绑定的是编译期 CPython 代理别名，
                    // 不进普通变量表（与 Import 同口径）：否则
                    // lookup_module_alias 会被一个 unknown 变量抢先命中。
                    if (imf.level == 0 && imf.module == "python") break;
                    for (auto& al : imf.names) bind_alias(al);
                    break;
                }
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

namespace {

// 语法预扫描：函数体还没检查时，前向引用的调用点也要能区分「协程」和
// 「异步生成器」。这里只看语句/表达式里可能出现的 yield，不进入嵌套
// def / class（它们的 yield 属于它们自己）。
bool expr_has_yield(const Expr* e);

bool exprs_have_yield(const std::vector<ExprPtr>& es) {
    for (auto& x : es)
        if (expr_has_yield(x.get())) return true;
    return false;
}

bool expr_has_yield(const Expr* e) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Yield:
        case ExprKind::YieldFrom: return true;
        case ExprKind::Await: return expr_has_yield(static_cast<const Await*>(e)->value.get());
        case ExprKind::BinOp: {
            auto& b = static_cast<const BinOp&>(*e);
            return expr_has_yield(b.left.get()) || expr_has_yield(b.right.get());
        }
        case ExprKind::UnaryOp:
            return expr_has_yield(static_cast<const UnaryOp&>(*e).operand.get());
        case ExprKind::BoolOp:
            return exprs_have_yield(static_cast<const BoolOp&>(*e).values);
        case ExprKind::Compare: {
            auto& c = static_cast<const Compare&>(*e);
            return expr_has_yield(c.left.get()) || exprs_have_yield(c.comparators);
        }
        case ExprKind::Call: {
            auto& c = static_cast<const Call&>(*e);
            if (expr_has_yield(c.func.get()) || exprs_have_yield(c.args)) return true;
            for (auto& kw : c.keywords)
                if (expr_has_yield(kw.value.get())) return true;
            return false;
        }
        case ExprKind::Attribute:
            return expr_has_yield(static_cast<const Attribute&>(*e).value.get());
        case ExprKind::Subscript: {
            auto& s = static_cast<const Subscript&>(*e);
            return expr_has_yield(s.value.get()) || expr_has_yield(s.slice.get());
        }
        case ExprKind::List:
            return exprs_have_yield(static_cast<const ListExpr&>(*e).elts);
        case ExprKind::Tuple:
            return exprs_have_yield(static_cast<const TupleExpr&>(*e).elts);
        case ExprKind::Set:
            return exprs_have_yield(static_cast<const SetExpr&>(*e).elts);
        case ExprKind::Dict: {
            auto& d = static_cast<const DictExpr&>(*e);
            return exprs_have_yield(d.keys) || exprs_have_yield(d.values);
        }
        case ExprKind::Starred:
            return expr_has_yield(static_cast<const Starred&>(*e).value.get());
        case ExprKind::IfExp: {
            auto& i = static_cast<const IfExp&>(*e);
            return expr_has_yield(i.test.get()) || expr_has_yield(i.body.get()) ||
                   expr_has_yield(i.orelse.get());
        }
        case ExprKind::NamedExpr:
            return expr_has_yield(static_cast<const NamedExpr&>(*e).value.get());
        default: return false;
    }
}

bool stmt_has_yield(const Stmt& s);

bool body_has_yield(const std::vector<StmtPtr>& body) {
    for (auto& s : body)
        if (s && stmt_has_yield(*s)) return true;
    return false;
}

bool stmt_has_yield(const Stmt& s) {
    switch (s.kind) {
        case StmtKind::Expr: return expr_has_yield(static_cast<const ExprStmt&>(s).value.get());
        case StmtKind::Assign: return expr_has_yield(static_cast<const Assign&>(s).value.get());
        case StmtKind::AnnAssign: {
            auto& a = static_cast<const AnnAssign&>(s);
            return a.value && expr_has_yield(a.value.get());
        }
        case StmtKind::AugAssign:
            return expr_has_yield(static_cast<const AugAssign&>(s).value.get());
        case StmtKind::Return: {
            auto& r = static_cast<const Return&>(s);
            return r.value && expr_has_yield(r.value.get());
        }
        case StmtKind::If: {
            auto& f = static_cast<const If&>(s);
            for (auto& b : f.branches)
                if (expr_has_yield(b.cond.get()) || body_has_yield(b.body)) return true;
            return body_has_yield(f.orelse);
        }
        case StmtKind::While: {
            auto& w = static_cast<const While&>(s);
            return expr_has_yield(w.cond.get()) || body_has_yield(w.body) ||
                   body_has_yield(w.orelse);
        }
        case StmtKind::For: {
            auto& f = static_cast<const For&>(s);
            return expr_has_yield(f.iter.get()) || body_has_yield(f.body) ||
                   body_has_yield(f.orelse);
        }
        case StmtKind::With: {
            auto& w = static_cast<const With&>(s);
            for (auto& it : w.items)
                if (expr_has_yield(it.context.get())) return true;
            return body_has_yield(w.body);
        }
        case StmtKind::Try: {
            auto& t = static_cast<const Try&>(s);
            if (body_has_yield(t.body) || body_has_yield(t.orelse) ||
                body_has_yield(t.finalbody))
                return true;
            for (auto& h : t.handlers)
                if (body_has_yield(h.body)) return true;
            return false;
        }
        case StmtKind::Match: {
            auto& m = static_cast<const Match&>(s);
            if (expr_has_yield(m.subject.get())) return true;
            for (auto& c : m.cases) {
                if (c.guard && expr_has_yield(c.guard.get())) return true;
                if (body_has_yield(c.body)) return true;
            }
            return false;
        }
        default: return false;
    }
}

}  // namespace

std::optional<LiteralValue> literal_of_expr(const Expr* e) {
    if (!e) return std::nullopt;
    // 负数字面量默认值（def f(size=-1)）：一元负/正包数字常量也是字面量。
    if (e->kind == ExprKind::UnaryOp) {
        const auto& u = static_cast<const UnaryOp&>(*e);
        if (u.op != Tok::MINUS && u.op != Tok::PLUS) return std::nullopt;
        auto inner = literal_of_expr(u.operand.get());
        if (!inner) return std::nullopt;
        if (u.op == Tok::PLUS) return inner;
        if (inner->kind == LiteralValue::Kind::Int) {
            // -u64大字面量 不是可表示的默认值（负数装不进 u64）；
            // 报错由 u64 字面量收尾检查负责（操作数已被 expr() 登记）。
            if (inner->u64) return std::nullopt;
            inner->i = -inner->i;
            return inner;
        }
        if (inner->kind == LiteralValue::Kind::Float) {
            inner->f = -inner->f;
            return inner;
        }
        if (inner->kind == LiteralValue::Kind::Big) {
            if (!inner->s.empty() && inner->s[0] == '-') inner->s.erase(0, 1);
            else inner->s.insert(inner->s.begin(), '-');
            return inner;
        }
        return std::nullopt;
    }
    if (e->kind != ExprKind::Constant) return std::nullopt;
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
            v.u64 = c.u64_lit;
            break;
        case Constant::CK::Float:
            v.kind = LiteralValue::Kind::Float;
            v.f = c.f;
            break;
        case Constant::CK::Str:
            v.kind = LiteralValue::Kind::Str;
            v.s = c.s;
            break;
        case Constant::CK::Bytes:
            v.kind = LiteralValue::Kind::Bytes;
            v.s = c.s;
            break;
        case Constant::CK::Big:
            v.kind = LiteralValue::Kind::Big;
            v.s = c.s;
            break;
        default: return std::nullopt;
    }
    return v;
}

FuncInfoPtr TypeChecker::build_sig(FunctionDef& f) {
        auto info = std::make_shared<FuncInfo>();
        info->name = f.name;
        auto& a = f.args;
        size_t total_pos = a.posonly.size() + a.args.size();
        size_t ndef = a.defaults.size();

        // 默认参数只收字面量；接口序列化需要不依赖 AST 的表示。
        auto literal_of = [](const Expr* value, const TypePtr& ty) {
            if (ty && ty->kind == TyKind::Big && value && value->kind == ExprKind::Constant) {
                const auto& c = static_cast<const Constant&>(*value);
                if (c.ck == Constant::CK::Int && c.u64_lit) {
                    LiteralValue lit;
                    lit.kind = LiteralValue::Kind::Big;
                    lit.s = std::to_string(static_cast<uint64_t>(c.i));
                    return std::optional<LiteralValue>(std::move(lit));
                }
            }
            return literal_of_expr(value);
        };

        // 默认值只作用于「最后 ndef 个位置参数」，与位置参数前缀无关
        auto add_pos = [&](Arg& arg, bool posonly) {
            Param p;
            p.name = arg.name;
            p.ty = parse_annotation(arg.annotation.get(), f.line);
            // 联合类型（kind 仍是 Unknown，带成员表）**不是**「显式 unknown」：
            // 它要按成员逐个检查实参，不能被 ann_unknown 的早退放行（0.488 Bug A）。
            p.ann_unknown = arg.annotation && p.ty && p.ty->kind == TyKind::Unknown &&
                            !is_union_ty(p.ty);
            size_t idx = info->pos.size();
            p.has_default = idx + ndef >= total_pos;
            p.kwonly = false;
            if (p.has_default) {
                size_t di = idx - (total_pos - ndef);
                if (di < a.defaults.size()) {
                    p.default_expr = a.defaults[di].get();
                    p.default_lit = literal_of(p.default_expr, p.ty);
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
            p.ann_unknown = a.kwonly[i].annotation && p.ty &&
                            p.ty->kind == TyKind::Unknown && !is_union_ty(p.ty);
            p.has_default = i < a.kw_defaults.size() && a.kw_defaults[i] != nullptr;
            p.kwonly = true;
            if (i < a.kw_defaults.size()) {
                p.default_expr = a.kw_defaults[i].get();
                p.default_lit = literal_of(p.default_expr, p.ty);
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
            // 先按语法扫一遍 yield：前向引用时也要把异步生成器标成
            // AsyncGenerator，而不是 Coroutine。
            if (body_has_yield(f.body)) {
                TypePtr gen = (info->ret && info->ret->kind == TyKind::AsyncGenerator)
                                  ? info->ret
                                  : nullptr;
                info->is_async_generator = true;
                info->ret = gen ? gen : mk_async_generator(t_unknown());
                info->async_result = t_none();
                return info;
            }
            // 调用 async def 得到协程对象；return/await 看到的是结果类型。
            info->async_result = info->ret;
            info->ret = mk_coroutine(info->ret ? info->ret : t_unknown());
        }
        return info;
    }

// 语句块是否必然离开：最后一条是 return/raise/break/continue，
// 或者是 if/else 且所有分支都离开。用来识别提前退出的 None 守卫。

void TypeChecker::err_member(const std::string& module, const std::string& name, int line,
                              int col) {
    if (auto msg = reject_message(module, name); !msg.empty()) {
        err(line, col, msg);
        return;
    }
    err(line, col, std::format("module '{}' has no attribute '{}'", module, name));
}

}  // namespace pyadamas
