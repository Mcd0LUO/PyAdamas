// lower_builtin.cpp —— 内置函数调用降级（builtin_call 与聚合辅助）
#include "lower_impl.h"

namespace pyadamas {

VReg Lowerer::lower_builtin_call(const std::string& name, Call& c) {
    // print 的关键字是 sep / end，得单独处理，不能走下面这条通用拦截
    if (name == "print") return lower_print(c);

    if (!c.keywords.empty() && name != "dict" && name != "open" && name != "sorted") {
        err(c.line, c.col, std::format("{}() does not take keyword arguments here", name));
    }

    // pa_repr_tuple 只认识 int/float/str/bool/None 五种元素。异构元组或
    // 元素是类/复数/range 时没有单一 kind，只能逐个取元素、按各自静态
    // 类型转文本再拼。CPython 的 str(tuple) 就是 repr(tuple)。
    auto tuple_repr_any = [&](auto&& self, TypePtr tt, VReg tv) -> VReg {
        auto scalar_kind = [](TypePtr et) -> int64_t {
            if (!et) return -1;
            switch (et->kind) {
                case TyKind::Int: return 0;
                case TyKind::Float: return 1;
                case TyKind::Str: return 2;
                case TyKind::Bool: return 3;
                case TyKind::None: return 4;
                default: return -1;
            }
        };
        auto concat = [&](VReg a, VReg b) {
            VReg s = call_runtime(RtFn::StrConcat, t_str(), {a, b});
            push_temp(s);
            return s;
        };
        VReg acc = const_str("(");
        for (size_t i = 0; i < tt->sub.size(); ++i) {
            if (i) acc = concat(acc, const_str(", "));
            TypePtr et = tt->sub[i];
            // 元组元素是容器内部的借用引用，不能交给临时栈释放。
            Inst get;
            get.op = IrOp::IndexLoad;
            get.a = tv;
            get.b = const_int(static_cast<int64_t>(i));
            VReg ev = def(et ? et : t_unknown(), std::move(get));
            int64_t k = scalar_kind(et);
            VReg piece = kNoReg;
            if (k == 2) {
                piece = call_runtime(RtFn::ReprStr, t_str(), {ev});
                push_temp(piece);
            } else if (k >= 0) {
                piece = convert_to(ev, et, t_str());
            } else if (et && et->kind == TyKind::Tuple) {
                int64_t ek = et->sub.empty() ? 0 : scalar_kind(et->sub[0]);
                bool homo = ek >= 0;
                for (size_t j = 1; homo && j < et->sub.size(); ++j)
                    if (scalar_kind(et->sub[j]) != ek) homo = false;
                if (et->variadic && ek < 0) {
                    err(c.line, c.col,
                        std::format("repr() of a variadic tuple with element type '{}' "
                                    "is not supported yet",
                                    type_name(et->sub[0])));
                    return const_str("");
                }
                if (homo) {
                    piece = call_runtime(RtFn::ReprTuple, t_str(), {ev, const_int(ek)});
                    push_temp(piece);
                } else {
                    piece = self(self, et, ev);
                }
            } else if (et && et->kind == TyKind::Range) {
                piece = call_runtime(RtFn::RangeRepr, t_str(), {ev});
                push_temp(piece);
            } else if (et && et->kind == TyKind::Complex) {
                piece = convert_to(ev, et, t_str());
            } else if (et && et->kind == TyKind::Class) {
                piece = dunder_str(ev, et, true);
                if (piece == kNoReg) piece = class_default_str(et);
            } else {
                err(c.line, c.col,
                    std::format("repr() of a tuple element of type '{}' is not "
                                "supported yet",
                                type_name(et)));
                return const_str("");
            }
            acc = concat(acc, piece);
        }
        if (tt->sub.size() == 1) acc = concat(acc, const_str(","));
        return concat(acc, const_str(")"));
    };

    // 同构且元素属于 pa_repr_tuple 支持的类型时走运行时快路径；
    // 其余固定长度元组逐元素拼接。变长元组只有元素同构，
    // 非标量元素无法在运行时按 kind 渲染。
    auto tuple_text = [&](TypePtr t, VReg v) -> VReg {
        TypePtr et = t->sub.empty() ? t_unknown() : t->sub[0];
        bool same = true;
        for (auto& sub : t->sub)
            if (!type_eq(sub, et)) { same = false; break; }
        bool scalar = et && (et->kind == TyKind::Int || et->kind == TyKind::Float ||
                             et->kind == TyKind::Str || et->kind == TyKind::Bool ||
                             et->kind == TyKind::None);
        if (same && scalar) {
            int64_t kind = 0;
            if (et->kind == TyKind::Float) kind = 1;
            else if (et->kind == TyKind::Str) kind = 2;
            else if (et->kind == TyKind::Bool) kind = 3;
            else if (et->kind == TyKind::None) kind = 4;
            VReg s = call_runtime(RtFn::ReprTuple, t_str(), {v, const_int(kind)});
            push_temp(s);
            return s;
        }
        if (t->variadic) {
            err(c.line, c.col,
                std::format("repr()/str() of a variadic tuple with element type '{}' "
                            "is not supported yet",
                            type_name(et)));
            return const_str("");
        }
        return tuple_repr_any(tuple_repr_any, t, v);
    };

    if (name == "len") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "len() takes exactly one argument");
            return const_int(0);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        VReg ln = dunder_len(v, t);
        if (ln != kNoReg) return ln;
        if (t && t->kind == TyKind::Range) {
            // range 的长度按边界算，不物化
            return call_runtime(RtFn::RangeLen, t_int(), {v});
        }
        if (t && t->kind == TyKind::Deque) {
            return call_runtime(RtFn::DequeLen, t_int(), {v});
        }
        if (t && t->kind == TyKind::Class) {
            err(c.line, c.col,
                std::format("len() of '{}' which has no __len__",
                            t->cls ? t->cls->name : "object"));
            return const_int(0);
        }
        Inst in;
        in.op = IrOp::Len;
        in.a = v;
        return def(t_int(), std::move(in));
    }

    if (name == "next") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "next() takes exactly one argument");
            return const_none();
        }
        TypePtr gt = tc_.type_of(c.args[0].get());
        if (gt && gt->kind == TyKind::File) {
            // next(f)：读一行，EOF 抛 StopIteration。
            VReg f = expr(*c.args[0]);
            VReg line = call_runtime(RtFn::FileReadLine, t_str(), {f, const_int(-1)});
            push_temp(line);
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = line;
            VReg n = def(t_int(), std::move(ln));
            Inst c0;
            c0.op = IrOp::Cmp;
            c0.cop = CmpKind::Eq;
            c0.a = n;
            c0.b = const_int(0);
            VReg empty = def(t_bool(), std::move(c0));
            int ok = new_block("next.ok");
            int stop = new_block("next.stop");
            Inst br;
            br.op = IrOp::Branch;
            br.a = empty;
            br.target = stop;
            br.target2 = ok;
            emit(std::move(br));
            switch_to(stop);
            call_void(RtFn::Raise, {const_str("StopIteration"), const_str("")});
            Inst j;
            j.op = IrOp::Jump;
            j.target = ok;
            emit(std::move(j));
            switch_to(ok);
            return line;
        }
        if (!gt || gt->kind != TyKind::Generator) {
            err(c.line, c.col, std::format("next() requires a generator, got '{}'", type_name(gt)));
            return const_none();
        }
        TypePtr et = gt->sub.empty() ? t_unknown() : gt->sub[0];
        VReg g = expr(*c.args[0]);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::GenNext);
        in.args = {g};
        // 运行时按位返回（浮点也是整数寄存器），vreg 类型记为 unknown；
        // 堆元素才入临时栈。
        VReg v = def(t_unknown(), std::move(in));
        v = retype(v, et);
        if (ir_is_heap(et)) push_temp(v);
        return v;
    }

    if (name == "open") {
        static const char* kParams[] = {"file",      "mode",    "buffering", "encoding",
                                        "errors",    "newline", "closefd",   "opener"};
        constexpr size_t kNP = 8;
        if (c.args.size() > kNP) {
            err(c.line, c.col, "open() takes at most 8 arguments");
            return const_none();
        }
        std::vector<VReg> bound(kNP, kNoReg);
        std::vector<Expr*> bexpr(kNP, nullptr);
        // 求值顺序按源码：位置实参先，关键字按出现顺序。
        for (size_t i = 0; i < c.args.size(); ++i) {
            bound[i] = expr(*c.args[i]);
            bexpr[i] = c.args[i].get();
        }
        for (auto& kw : c.keywords) {
            if (kw.arg.empty()) {
                err(kw.value->line, kw.value->col, "open() does not take '**kwargs'");
                continue;
            }
            int idx = -1;
            for (int p = 0; p < static_cast<int>(kNP); ++p)
                if (kw.arg == kParams[p]) idx = p;
            VReg v = expr(*kw.value);
            if (idx < 0) {
                err(kw.value->line, kw.value->col,
                    std::format("open() got an unexpected keyword argument '{}'", kw.arg));
                continue;
            }
            if (bound[static_cast<size_t>(idx)] != kNoReg) {
                err(kw.value->line, kw.value->col,
                    std::format("open() got multiple values for argument '{}'", kw.arg));
                continue;
            }
            bound[static_cast<size_t>(idx)] = v;
            bexpr[static_cast<size_t>(idx)] = kw.value.get();
        }
        if (bound[0] == kNoReg) {
            err(c.line, c.col, "open() missing required argument 'file'");
            return const_none();
        }
        if (bound[7] != kNoReg) {
            err(c.line, c.col, "open() opener= is not supported by the code generator");
            return const_none();
        }
        TypePtr ft = tc_.type_of(bexpr[0]);
        if (!ft || (ft->kind != TyKind::Str && ft->kind != TyKind::Int &&
                    ft->kind != TyKind::Bool)) {
            err(c.line, c.col, "open() file must be a statically known path or file descriptor");
            return const_none();
        }
        bool is_fd = ft->kind == TyKind::Int || ft->kind == TyKind::Bool;
        VReg mode = bound[1] == kNoReg ? const_int(0) : bound[1];
        VReg buffering = bound[2] == kNoReg
                             ? const_int(-1)
                             : coerce(bound[2], tc_.type_of(bexpr[2]), t_int());
        // newline: None→0(通用换行), ""→1, "\n"→2, "\r"→3, "\r\n"→4
        int64_t nl = 0;
        if (bexpr[5]) {
            const Constant* cc =
                bexpr[5]->kind == ExprKind::Constant ? static_cast<const Constant*>(bexpr[5])
                                                     : nullptr;
            if (cc && cc->ck == Constant::CK::None) {
                nl = 0;
            } else if (cc && cc->ck == Constant::CK::Str) {
                if (cc->s.empty()) nl = 1;
                else if (cc->s == "\n") nl = 2;
                else if (cc->s == "\r") nl = 3;
                else if (cc->s == "\r\n") nl = 4;
                else {
                    err(bexpr[5]->line, bexpr[5]->col,
                        "open() newline must be None, '', '\\n', '\\r' or '\\r\\n'");
                    nl = 1;
                }
            } else {
                err(bexpr[5]->line, bexpr[5]->col,
                    "open() newline must be a literal (None, '', '\\n', '\\r' or '\\r\\n')");
            }
        }
        VReg closefd = bound[6] == kNoReg
                           ? const_int(1)
                           : coerce(bound[6], tc_.type_of(bexpr[6]), t_int());
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::FileOpen);
        in.args = {is_fd ? const_int(0) : bound[0], is_fd ? bound[0] : const_int(-1), mode,
                   buffering, const_int(nl), closefd};
        in.ty = t_file();
        VReg f = def(t_file(), std::move(in));
        push_temp(f);
        // encoding/errors 只在实际给了的时候再配置一次（默认 utf-8/strict）。
        if (bound[3] != kNoReg || bound[4] != kNoReg) {
            call_void(RtFn::FileConfig,
                      {f, bound[3] == kNoReg ? const_int(0) : bound[3],
                       bound[4] == kNoReg ? const_int(0) : bound[4]});
        }
        return f;
    }

    if (name == "input") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "input() takes at most one argument");
            return const_str("");
        }
        VReg prompt = c.args.empty() ? const_int(0) : expr(*c.args[0]);
        VReg s = call_runtime(RtFn::Input, t_str(), {prompt});
        push_temp(s);
        return s;
    }

    if (name == "iter") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "iter() takes exactly one argument");
            return const_none();
        }
        TypePtr it = tc_.type_of(c.args[0].get());
        if (it && (it->kind == TyKind::File || it->kind == TyKind::List ||
                   it->kind == TyKind::Tuple || it->kind == TyKind::Str ||
                   it->kind == TyKind::Bytes || it->kind == TyKind::Dict ||
                   it->kind == TyKind::Set || it->kind == TyKind::Range ||
                   it->kind == TyKind::Generator)) {
            return expr(*c.args[0]);
        }
        if (it && it->kind == TyKind::Class) {
            VReg seq = expr(*c.args[0]);
            VReg r = dunder_call(seq, it, "__iter__", {}, nullptr);
            if (r != kNoReg) return r;
            err(c.args[0]->line, c.args[0]->col,
                std::format("iter() of '{}' which has no usable __iter__",
                            type_name(it)));
            return const_none();
        }
        if (!it || it->kind == TyKind::Unknown) return expr(*c.args[0]);
        err(c.line, c.col,
            std::format("iter() of '{}' is not supported by the code generator yet",
                        type_name(it)));
        return const_none();
    }

    if (name == "isinstance") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "isinstance() takes exactly two arguments");
            return const_bool(false);
        }
        TypePtr vt = tc_.type_of(c.args[0].get());
        TypePtr tt = tc_.type_of(c.args[1].get());
        if (tt && tt->kind == TyKind::ClassObj && !tt->sub.empty()) tt = tt->sub[0];
        TypePtr st = strip_optional(vt);
        std::string builtin;
        if (tt && tt->kind == TyKind::Func && tt->fn) builtin = tt->fn->name;
        if (!builtin.empty()) {
            if (builtin == "object") return const_bool(true);
            bool known = st && st->kind != TyKind::Unknown;
            auto kind_is = [&](std::initializer_list<TyKind> ks) {
                for (auto k : ks)
                    if (st && st->kind == k) return true;
                return false;
            };
            if (known) {
                bool r = false;
                if (builtin == "int") r = kind_is({TyKind::Int, TyKind::Bool});
                else if (builtin == "bool") r = kind_is({TyKind::Bool});
                else if (builtin == "float") r = kind_is({TyKind::Float});
                else if (builtin == "str") r = kind_is({TyKind::Str});
                else if (builtin == "list") r = kind_is({TyKind::List});
                else if (builtin == "tuple") r = kind_is({TyKind::Tuple});
                else if (builtin == "dict") r = kind_is({TyKind::Dict});
                else if (builtin == "set") r = kind_is({TyKind::Set});
                else if (builtin == "complex") r = kind_is({TyKind::Complex});
                else if (builtin == "range") r = kind_is({TyKind::Range});
                else {
                    err(c.args[1]->line, c.args[1]->col,
                        std::format("isinstance() does not support type '{}' yet", builtin));
                    return const_bool(false);
                }
                return const_bool(r);
            }
            // unknown 值：只有带运行期类型标签的容器/标量能查。
            RtFn fn{};
            if (builtin == "str") fn = RtFn::IsStr;
            else if (builtin == "list" || builtin == "tuple") fn = RtFn::IsSeq;
            else if (builtin == "dict") fn = RtFn::IsDict;
            else if (builtin == "complex") fn = RtFn::IsComplex;
            else {
                err(c.args[1]->line, c.args[1]->col,
                    std::format("isinstance() of an unknown value as '{}' is not supported yet",
                                builtin));
                return const_bool(false);
            }
            return call_runtime(fn, t_bool(), {expr(*c.args[0])});
        }
        ClassInfo* target = tt && tt->kind == TyKind::Class ? tt->cls : nullptr;
        if (!target) {
            err(c.args[1]->line, c.args[1]->col,
                "isinstance() second argument must be a class or builtin type");
            return const_bool(false);
        }
        if (target->is_exception_class) {
            if (st && st->kind != TyKind::Unknown && st->kind != TyKind::Exception)
                return const_bool(false);
            return call_runtime(RtFn::IsException, t_bool(),
                                {expr(*c.args[0]), const_str(target->name)});
        }
        auto cid = class_ids_.find(target);
        if (cid == class_ids_.end()) {
            // 导入类没有本模块的运行期编号，用「模块名.类名」比较；
            // pa_is_named_class 会沿实例的动态 MRO 查，子类也能匹配。
            std::string qual = target->origin_qualname;
            if (qual.empty()) {
                auto scan = [&](const std::unordered_map<std::string, ModuleInfoPtr>& mods) {
                    for (auto& entry : mods) {
                        if (qual.empty() == false) return;
                        for (auto& ce : entry.second->classes)
                            if (ce.second.cls.get() == target) {
                                qual = entry.second->name + "." + ce.first;
                                return;
                            }
                    }
                };
                scan(st_->modules);
                if (qual.empty()) scan(module_aliases_);
            }
            if (qual.empty()) {
                err(c.args[1]->line, c.args[1]->col,
                    "isinstance() with an imported class is not supported yet");
                return const_bool(false);
            }
            return call_runtime(RtFn::IsNamedClass, t_bool(),
                                {expr(*c.args[0]), const_str(qual)});
        }
        bool runtime = true;
        if (st && st->kind == TyKind::Class && st->cls) {
            bool a = st->cls->is_subclass_of(target);
            bool b = target->is_subclass_of(st->cls);
            if (a)
                runtime = vt != st;  // Optional 可能实际是 None
            else if (b)
                runtime = true;      // 值可能是子类
            else
                return const_bool(false);
        } else if (st && st->kind != TyKind::Unknown) {
            return const_bool(false);
        }
        if (!runtime) return const_bool(true);
        // 类编号只在模块内唯一，跨模块对象 MRO 里可能有同号的别的类；
        // 本地类目标也按限定名比较，避免误匹配。
        std::string qual = mod_.runtime_name + "." + target->name;
        return call_runtime(RtFn::IsNamedClass, t_bool(),
                            {expr(*c.args[0]), const_str(qual)});
    }

    if (name == "type") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "type() takes exactly one argument");
            return const_none();
        }
        Expr* arg = c.args[0].get();
        TypePtr t = tc_.type_of(arg);
        bool skip_eval = false;
        if (arg->kind == ExprKind::Name) {
            auto& id = static_cast<Name*>(arg)->id;
            skip_eval = !is_bound_here(id) &&
                        (classes_.count(id) || imported_classes_.count(id) ||
                         TypeChecker::is_builtin_name(id));
        }
        if (!skip_eval) (void)expr(*arg);  // 保留求值副作用
        VReg s = const_str(py_type_name(t));
        return retype(s, mk_typeobj(t ? t : t_unknown()));
    }

    if (name == "id") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "id() takes exactly one argument");
            return const_int(0);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (t && t->kind == TyKind::Float)
            return call_runtime(RtFn::HashF64, t_int(), {v});
        return retype(v, t_int());
    }

    if (name == "hash") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "hash() takes exactly one argument");
            return const_int(0);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (t && (t->kind == TyKind::Int || t->kind == TyKind::Bool))
            return call_runtime(RtFn::HashValue, t_int(), {const_int(0), v});
        if (t && t->kind == TyKind::Str)
            return call_runtime(RtFn::HashValue, t_int(), {const_int(1), v});
        if (t && t->kind == TyKind::Float)
            return call_runtime(RtFn::HashF64, t_int(), {v});
        if (t && t->kind == TyKind::Class) {
            VReg h = dunder_call(v, t, "__hash__", {}, t_int());
            if (h != kNoReg) return h;
            // 没有 __hash__：定义了 __eq__ 的类按 CPython 规则不可哈希；
            // 否则退回身份哈希（和 dict/set 对类键的指针哈希一致）。
            const MethodInfo* eq = t->cls ? t->cls->method("__eq__") : nullptr;
            if (eq && eq->kind == MethodKind::Normal) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("unhashable type: '{}' defines __eq__ but no __hash__",
                                type_name(t)));
                return const_int(0);
            }
            return call_runtime(RtFn::HashValue, t_int(), {const_int(3), v});
        }
        err(c.args[0]->line, c.args[0]->col,
            std::format("hash() of '{}' is not supported yet (int/bool/str/float)",
                        type_name(t)));
        return const_int(0);
    }

    if (name == "callable") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "callable() takes exactly one argument");
            return const_bool(false);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        bool is_class = t && t->kind == TyKind::Class;
        bool is_builtin_name = false;
        if (c.args[0]->kind == ExprKind::Name) {
            auto& id = static_cast<Name*>(c.args[0].get())->id;
            is_builtin_name = TypeChecker::is_builtin_name(id) && !is_local(id);
        }
        if (!is_class && !is_builtin_name) (void)expr(*c.args[0]);  // 求值副作用
        if (!t || t->kind == TyKind::Unknown) {
            err(c.args[0]->line, c.args[0]->col,
                "callable() of an unknown value is not supported yet");
            return const_bool(false);
        }
        return const_bool(is_class || is_builtin_name || t->kind == TyKind::Func);
    }

    if (name == "issubclass") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "issubclass() takes exactly two arguments");
            return const_bool(false);
        }
        TypePtr a0 = tc_.type_of(c.args[0].get());
        TypePtr a1 = tc_.type_of(c.args[1].get());
        if (a0 && a0->kind == TyKind::ClassObj && !a0->sub.empty()) a0 = a0->sub[0];
        if (a1 && a1->kind == TyKind::ClassObj && !a1->sub.empty()) a1 = a1->sub[0];
        auto builtin = [](const TypePtr& t) -> std::string {
            if (t && t->kind == TyKind::Func && t->fn && t->fn->unchecked)
                return t->fn->name;
            return std::string();
        };
        ClassInfo* ca = a0 && a0->kind == TyKind::Class ? a0->cls : nullptr;
        ClassInfo* cb = a1 && a1->kind == TyKind::Class ? a1->cls : nullptr;
        std::string ba = builtin(a0);
        std::string bb = builtin(a1);
        if ((!ca && ba.empty()) || (!cb && bb.empty())) {
            err(c.line, c.col,
                "issubclass() arguments must be classes or builtin types");
            return const_bool(false);
        }
        bool r = false;
        if (ca && cb) {
            r = ca->is_subclass_of(cb);
        } else if (ca) {
            r = bb == "object";
        } else if (cb) {
            r = false;
        } else if (bb == "object") {
            r = true;
        } else if (bb == "int") {
            r = ba == "int" || ba == "bool";
        } else {
            r = ba == bb;
        }
        return const_bool(r);
    }

    if (name == "hasattr" || name == "getattr" || name == "setattr") {
        size_t want = name == "setattr" ? 3 : 2;
        if (c.args.size() != want) {
            err(c.line, c.col, std::format("{}() takes exactly {} arguments", name, want));
            return name == "hasattr" ? const_bool(false) : const_none();
        }
        const std::string* an = nullptr;
        Expr* name_expr = c.args[1].get();
        if (name_expr->kind == ExprKind::Constant) {
            auto& cc = static_cast<Constant&>(*name_expr);
            if (cc.ck == Constant::CK::Str) an = &cc.s;
        }
        if (!an) {
            err(name_expr->line, name_expr->col,
                std::format("{}() attribute name must be a string literal", name));
            return name == "hasattr" ? const_bool(false) : const_none();
        }
        TypePtr raw = tc_.type_of(c.args[0].get());
        TypePtr ot = strip_optional(raw);
        if (!ot || ot->kind != TyKind::Class || !ot->cls) {
            // hasattr 对已知的内建/容器类型可以静态判定；Optional 的堆类型
            // 再补一个非空检查（None 就是 0）。
            if (name == "hasattr" && ot && ot->kind != TyKind::Unknown) {
                bool has = TypeChecker::type_has_member(ot, *an);
                VReg v = expr(*c.args[0]);
                if (!has) return const_bool(false);
                if (raw != ot && ir_is_heap(ot)) {
                    Inst cmp;
                    cmp.op = IrOp::Cmp;
                    cmp.cop = CmpKind::Ne;
                    cmp.a = retype(v, t_int());  // 按裸指针判空，避免 str 内容比较
                    cmp.b = const_int(0);
                    return def(t_bool(), std::move(cmp));
                }
                return const_bool(true);
            }
            err(c.args[0]->line, c.args[0]->col,
                std::format("{}() is only supported on class instances", name));
            return name == "hasattr" ? const_bool(false) : const_none();
        }
        const FieldInfo* f = ot->cls->field(*an);
        const PropertyInfo* prop = ot->cls->property(*an);
        auto* meth = ot->cls->method(*an);
        bool optional = ot != raw;
        if (name == "hasattr") {
            if (optional) {
                err(c.line, c.col,
                    "hasattr() on an Optional value is not supported yet");
                return const_bool(false);
            }
            (void)expr(*c.args[0]);  // 保留求值副作用
            return const_bool(f || prop || meth);
        }
        VReg obj = expr(*c.args[0]);
        if (optional) obj = guard_not_none(obj, std::format("attribute '{}'", *an));
        if (name == "getattr") {
            if (prop) {
                if (!prop->owner || prop->getter < 0 ||
                    prop->getter >= static_cast<int>(prop->owner->methods.size())) {
                    err(c.line, c.col, std::format("property '{}' has no getter", *an));
                    return const_none();
                }
                ++side_effect_seq_;
                const MethodInfo& gm = prop->owner->methods[prop->getter];
                Inst vc;
                vc.op = IrOp::VCall;
                vc.a = obj;
                vc.imm = gm.slot;
                TypePtr rt = gm.call_sig && gm.call_sig->ret ? gm.call_sig->ret : t_unknown();
                return def_heap(rt, std::move(vc));
            }
            if (!f) {
                if (meth)
                    err(c.line, c.col,
                        std::format("getattr() of method '{}' must be called with ()", *an));
                else
                    err(c.line, c.col,
                        std::format("'{}' object has no attribute '{}'", ot->cls->name, *an));
                return const_none();
            }
            int off = static_cast<int>(8 * (1 + ot->cls->field_index(*an)));
            return field_load(obj, off, f->ty);
        }
        // setattr
        if (!f) {
            if (prop) {
                if (!prop->owner || prop->setter < 0 ||
                    prop->setter >= static_cast<int>(prop->owner->methods.size())) {
                    err(c.line, c.col, std::format("property '{}' has no setter", *an));
                    return const_none();
                }
                const MethodInfo& sm = prop->owner->methods[prop->setter];
                VReg val = expr(*c.args[2]);
                TypePtr want = sm.call_sig && !sm.call_sig->pos.empty()
                                   ? sm.call_sig->pos[0].ty
                                   : t_unknown();
                val = coerce(val, tc_.type_of(c.args[2].get()), want);
                Inst vc;
                vc.op = IrOp::VCall;
                vc.a = obj;
                vc.imm = sm.slot;
                vc.args = {val};
                emit(std::move(vc));
                return const_none();
            }
            err(c.line, c.col,
                std::format("'{}' object has no field '{}'", ot->cls->name, *an));
            return const_none();
        }
        VReg v = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), f->ty);
        int off = static_cast<int>(8 * (1 + ot->cls->field_index(*an)));
        store_field(f->ty, obj, off, v);
        return const_none();
    }

    if (name == "abs") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "abs() takes exactly one argument");
            return const_none();
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (t && t->kind == TyKind::Float)
            return call_runtime(RtFn::MathFabs, t_float(), {v});
        if (t && t->kind == TyKind::Complex)
            return call_runtime(RtFn::ComplexAbs, t_float(), {v});
        return call_runtime(RtFn::AbsI64, t_int(), {v});
    }

    if (name == "min" || name == "max") return lower_minmax(c, name == "min");
    if (name == "sum") return lower_sum(c);
    if (name == "any" || name == "all") return lower_anyall(c, name == "any");
    if (name == "complex") {
        if (c.args.size() > 2) {
            err(c.line, c.col, "complex() takes at most two arguments");
            return const_none();
        }
        VReg re = const_float(0.0);
        VReg im = const_float(0.0);
        if (!c.args.empty()) {
            TypePtr t0 = tc_.type_of(c.args[0].get());
            VReg v0 = expr(*c.args[0]);
            if (t0 && t0->kind == TyKind::Complex && c.args.size() == 1) return v0;
            re = (t0 && t0->kind == TyKind::Float) ? v0 : convert_to(v0, t0, t_float());
        }
        if (c.args.size() == 2) {
            TypePtr t1 = tc_.type_of(c.args[1].get());
            VReg v1 = expr(*c.args[1]);
            im = (t1 && t1->kind == TyKind::Float) ? v1 : convert_to(v1, t1, t_float());
        }
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::ComplexNew);
        in.args = {re, im};
        return def_heap(t_complex(), std::move(in));
    }
    if (name == "pow") {
        if (c.args.size() != 2 && c.args.size() != 3) {
            err(c.line, c.col, "pow() takes 2 or 3 arguments");
            return const_none();
        }
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind == TyKind::Unknown) ty = t_int();
        if (c.args.size() == 3) {
            VReg a = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
            VReg b = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            VReg m = coerce(expr(*c.args[2]), tc_.type_of(c.args[2].get()), t_int());
            Inst pw;
            pw.op = IrOp::Bin;
            pw.bop = BinKind::Pow;
            pw.a = a;
            pw.b = b;
            VReg p = def(t_int(), std::move(pw));
            Inst md;
            md.op = IrOp::Bin;
            md.bop = BinKind::Mod;
            md.a = p;
            md.b = m;
            return def(t_int(), std::move(md));
        }
        VReg a = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), ty);
        VReg b = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), ty);
        Inst in;
        in.op = IrOp::Bin;
        in.bop = BinKind::Pow;
        in.a = a;
        in.b = b;
        return def(ty, std::move(in));
    }

    if (name == "ord") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "ord() takes exactly one argument");
            return const_int(0);
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        if (!t || (t->kind != TyKind::Str && t->kind != TyKind::Unknown)) {
            err(c.line, c.col, std::format("ord() expects a string, got '{}'", type_name(t)));
            return const_int(0);
        }
        VReg v = expr(*c.args[0]);
        return call_runtime(RtFn::Ord, t_int(), {v});
    }

    if (name == "__pa_capwords") {
        if (c.args.size() != 1 || !c.keywords.empty()) {
            err(c.line, c.col, "capwords() takes exactly one argument");
            return const_str("");
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        if (t && t->kind != TyKind::Str && t->kind != TyKind::Unknown) {
            err(c.line, c.col,
                std::format("capwords() expects a string, got '{}'", type_name(t)));
            return const_str("");
        }
        VReg v = expr(*c.args[0]);
        VReg r = call_runtime(RtFn::StrCapwords, t_str(), {v});
        push_temp(r);
        return r;
    }

    if (name == "chr") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "chr() takes exactly one argument");
            return const_str("");
        }
        VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg s = call_runtime(RtFn::Chr, t_str(), {v});
        push_temp(s);
        return s;
    }

    if (name == "hex" || name == "bin" || name == "oct") {
        if (c.args.size() != 1) {
            err(c.line, c.col, std::format("{}() takes exactly one argument", name));
            return const_str("");
        }
        int64_t base = name == "hex" ? 16 : name == "bin" ? 2 : 8;
        VReg v = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), t_int());
        VReg s = call_runtime(RtFn::IntToBase, t_str(), {v, const_int(base)});
        push_temp(s);
        return s;
    }

    if (name == "round") {
        if (c.args.empty() || c.args.size() > 2) {
            err(c.line, c.col, "round() takes 1 or 2 arguments");
            return const_int(0);
        }
        TypePtr at = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (at && at->kind == TyKind::Float) {
            VReg nd = c.args.size() == 2
                          ? coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int())
                          : const_int(0);
            VReg r = call_runtime(RtFn::RoundF64, t_float(), {v, nd});
            // round(x) 返回 int；round(x, n) 仍返回 float。
            if (c.args.size() == 1) return convert_to(r, t_float(), t_int());
            return r;
        }
        // 整数输入：单参就是它自己；带负 ndigits 要按 10 的幂取整。
        if (c.args.size() == 2 && at &&
            (at->kind == TyKind::Int || at->kind == TyKind::Bool)) {
            VReg nd = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            return call_runtime(RtFn::RoundI64, t_int(), {v, nd});
        }
        return v;
    }

    if (name == "divmod") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "divmod() takes exactly two arguments");
            return const_none();
        }
        TypePtr ty = tc_.type_of(&c);
        TypePtr et = t_int();
        if (ty && ty->kind == TyKind::Tuple && !ty->sub.empty()) et = ty->sub[0];
        VReg a = coerce(expr(*c.args[0]), tc_.type_of(c.args[0].get()), et);
        VReg b = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), et);
        Inst q;
        q.op = IrOp::Bin;
        q.bop = BinKind::FloorDiv;
        q.a = a;
        q.b = b;
        VReg qv = def(et, std::move(q));
        Inst m;
        m.op = IrOp::Bin;
        m.bop = BinKind::Mod;
        m.a = a;
        m.b = b;
        VReg mv = def(et, std::move(m));
        Inst mt;
        mt.op = IrOp::MakeTuple;
        mt.imm = !et ? 2 : et->kind == TyKind::Unknown ? 2 : (ir_is_heap(et) ? 1 : 0);
        mt.args = {qv, mv};
        return def_heap(ty && ty->kind == TyKind::Tuple ? ty : mk_tuple({et, et}),
                        std::move(mt));
    }

    if (name == "repr") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "repr() takes exactly one argument");
            return const_str("");
        }
        TypePtr t = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (t && t->kind == TyKind::Str) {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(RtFn::ReprStr);
            in.args = {v};
            return def_heap(t_str(), std::move(in));
        }
        if (t && t->kind == TyKind::Range) {
            // repr(range(10)) 是 "range(0, 10)"，和 CPython 一样
            VReg s = call_runtime(RtFn::RangeRepr, t_str(), {v});
            push_temp(s);
            return s;
        }
        if (t && t->kind == TyKind::Tuple) return tuple_text(t, v);
        if (t && t->kind == TyKind::ClassObj) {
            VReg s = call_runtime(RtFn::ClassRepr, t_str(), {v});
            push_temp(s);
            return s;
        }
        if (t && (t->kind == TyKind::Int || t->kind == TyKind::Bool ||
                  t->kind == TyKind::Float || t->kind == TyKind::Complex ||
                  t->kind == TyKind::None))
            return convert_to(v, t, t_str());
        if (t && t->kind == TyKind::Class) {
            VReg s = dunder_str(v, t, true);
            return s != kNoReg ? s : class_default_str(t);
        }
        err(c.line, c.col,
            std::format("repr() of '{}' is not supported by the code generator yet",
                        type_name(t)));
        return const_str("");
    }

    if (name == "format") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "format() takes exactly two arguments");
            return const_str("");
        }
        TypePtr vt = tc_.type_of(c.args[0].get());
        if (vt && vt->kind == TyKind::Class && vt->cls &&
            vt->cls->method("__format__")) {
            VReg v = expr(*c.args[0]);
            VReg spec = expr(*c.args[1]);
            if (spec == kNoReg) return const_str("");
            VReg r = dunder_call(v, vt, "__format__", {spec}, t_str());
            if (r == kNoReg) {
                err(c.line, c.col,
                    std::format("__format__ on '{}' must be a normal method",
                                vt->cls->name));
                return const_str("");
            }
            return r;
        }
        RtFn fn = RtFn::FmtI64;
        if (vt && vt->kind == TyKind::Float) fn = RtFn::FmtF64;
        else if (vt && vt->kind == TyKind::Bool) fn = RtFn::FmtBool;
        else if (vt && vt->kind == TyKind::Str) fn = RtFn::FmtStr;
        else if (vt && vt->kind == TyKind::Complex) fn = RtFn::FmtComplex;
        else if (!vt || (vt->kind != TyKind::Int && vt->kind != TyKind::Unknown)) {
            err(c.line, c.col,
                std::format("format() of '{}' is not supported yet", type_name(vt)));
            return const_str("");
        }
        VReg v = expr(*c.args[0]);
        VReg spec = expr(*c.args[1]);
        if (spec == kNoReg) return const_str("");
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(fn);
        in.args = {v, spec};
        return def_heap(t_str(), std::move(in));
    }

    if (name == "range") {
        if (c.args.empty() || c.args.size() > 3) {
            err(c.line, c.col, "range() takes 1 to 3 arguments");
            return const_none();
        }
        auto arg = [&](size_t i) {
            VReg v = expr(*c.args[i]);
            return coerce(v, tc_.type_of(c.args[i].get()), t_int());
        };
        VReg start = const_int(0);
        VReg stop = arg(0);
        VReg step = const_int(1);
        if (c.args.size() >= 2) {
            start = stop;
            stop = arg(1);
        }
        if (c.args.size() == 3) step = arg(2);
        VReg v = call_runtime(RtFn::RangeNew, mk_range(t_int()), {start, stop, step});
        push_temp(v);
        return v;
    }

    if (name == "int" || name == "float" || name == "str" || name == "bool") {
        TypePtr to = name == "int"     ? t_int()
                     : name == "float" ? t_float()
                     : name == "str"   ? t_str()
                                       : t_bool();
        if (c.args.empty()) return to->kind == TyKind::Int ? const_int(0)
                                       : to->kind == TyKind::Float ? const_float(0)
                                       : to->kind == TyKind::Str   ? const_str("")
                                                                   : const_bool(false);
        // int(str, base)
        if (name == "int" && c.args.size() == 2) {
            TypePtr st = tc_.type_of(c.args[0].get());
            VReg sv = expr(*c.args[0]);
            if (!st || st->kind != TyKind::Str) {
                err(c.line, c.col, "int(x, base) requires a string first argument");
                return const_int(0);
            }
            VReg bv = coerce(expr(*c.args[1]), tc_.type_of(c.args[1].get()), t_int());
            return call_runtime(RtFn::StrToI64, t_int(), {sv, bv});
        }
        if (c.args.size() > 1) {
            err(c.line, c.col, std::format("{}() takes at most one argument", name));
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        TypePtr from = tc_.type_of(c.args[0].get());
        // 字符串解析：以前走 Convert 会把字符串指针直接当数值，静默算错。
        if (name == "int" && from && from->kind == TyKind::Str)
            return call_runtime(RtFn::StrToI64, t_int(), {v, const_int(10)});
        if (name == "float" && from && from->kind == TyKind::Str)
            return call_runtime(RtFn::StrToF64, t_float(), {v});
        if (to->kind == TyKind::Str && from && from->kind == TyKind::Exception) {
            VReg s = call_runtime(RtFn::ExcValueStr, t_str(), {v});
            push_temp(s);
            return s;
        }
        if (to->kind == TyKind::Str && from && from->kind == TyKind::Tuple) {
            // str(tuple) 与 repr(tuple) 相同（CPython 也是这样）
            return tuple_text(from, v);
        }
        if (to->kind == TyKind::Str && from && from->kind == TyKind::ClassObj) {
            VReg s = call_runtime(RtFn::ClassRepr, t_str(), {v});
            push_temp(s);
            return s;
        }
        // 容器转标量没有定义（CPython 抛 TypeError）。以前这里直接把
        // 指针当数字用，会静默给出垃圾值——宁可报错。
        if (to->kind != TyKind::Bool && from &&
            (from->kind == TyKind::List || from->kind == TyKind::Dict ||
             from->kind == TyKind::Set ||
             (from->kind == TyKind::Range && to->kind != TyKind::Str))) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("{}() argument must be a number or a string, not '{}'",
                            name, type_name(from)));
            return to->kind == TyKind::Int ? const_int(0)
                   : to->kind == TyKind::Float ? const_float(0.0)
                   : to->kind == TyKind::Str ? const_str("") : const_bool(false);
        }
        if (to->kind == TyKind::Str && from && from->kind == TyKind::Range) {
            // str(range) 与 repr(range) 相同
            VReg s = call_runtime(RtFn::RangeRepr, t_str(), {v});
            push_temp(s);
            return s;
        }
        return convert_to(v, from, to);
    }

    if (name == "list" || name == "tuple") {
        TypePtr ty = tc_.type_of(&c);
        bool as_tuple = name == "tuple";
        auto result_ty = [&]() -> TypePtr {
            if (ty) return ty;
            return as_tuple ? mk_tuple({}) : mk_list(t_unknown());
        };
        if (c.args.empty()) {
            if (as_tuple) {
                Inst mt;
                mt.op = IrOp::MakeTuple;
                mt.imm = list_kind(ty);
                mt.ty = result_ty();
                return def_heap(result_ty(), std::move(mt));
            }
            VReg l = call_runtime(RtFn::ListNew, result_ty(),
                                  {const_int(0), const_int(list_kind(ty))});
            push_temp(l);
            return l;
        }
        if (c.args.size() != 1) {
            err(c.line, c.col, std::format("{}() takes at most one argument", name));
            return const_none();
        }
        TypePtr xt = tc_.type_of(c.args[0].get());
        if (xt && xt->kind == TyKind::Deque) {
            VReg d = expr(*c.args[0]);
            VReg xs = call_runtime(RtFn::DequeToList, mk_list(t_int()), {d});
            push_temp(xs);
            return xs;
        }
        if (!xt || (xt->kind != TyKind::List && xt->kind != TyKind::Tuple &&
                    xt->kind != TyKind::Str && xt->kind != TyKind::Bytes &&
                    xt->kind != TyKind::Dict &&
                    xt->kind != TyKind::Set && xt->kind != TyKind::File &&
                    xt->kind != TyKind::Range && xt->kind != TyKind::Generator)) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("{}() cannot iterate over '{}'", name, type_name(xt)));
            return const_none();
        }
        TypePtr et = t_unknown();
        VReg x = seq_as_list(c.args[0].get(), et);
        // str/dict/set/生成器 走 seq_as_list 时已经是新建的列表，不用再拷。
        bool fresh = xt->kind == TyKind::Str || xt->kind == TyKind::Bytes ||
                     xt->kind == TyKind::Dict ||
                     xt->kind == TyKind::Set || xt->kind == TyKind::Generator;
        VReg l = fresh ? retype(x, result_ty())
                       : call_runtime(RtFn::ListCopy, result_ty(), {x});
        if (!fresh) push_temp(l);
        return as_tuple ? retype(l, result_ty()) : l;
    }

    if (name == "dict") {
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::Dict) ty = mk_dict(t_unknown(), t_unknown());
        TypePtr kt = t_unknown(), vt = t_unknown();
        if (!ty->sub.empty()) kt = ty->sub[0];
        if (ty->sub.size() > 1) vt = ty->sub[1];
        if (!c.args.empty()) {
            if (c.args.size() != 1 || !c.keywords.empty()) {
                err(c.line, c.col, "dict() takes at most one positional argument");
                return const_none();
            }
            TypePtr xt = tc_.type_of(c.args[0].get());
            if (!xt || (xt->kind != TyKind::List && xt->kind != TyKind::Tuple &&
                        xt->kind != TyKind::Set && xt->kind != TyKind::Generator &&
                        xt->kind != TyKind::File && xt->kind != TyKind::Range)) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("dict() cannot iterate over '{}'", type_name(xt)));
                return const_none();
            }
            TypePtr se = t_unknown();
            VReg xs = seq_as_list(c.args[0].get(), se);
            auto [hslot, eslot] = dunder_key_slots(kt);
            VReg d = call_runtime(RtFn::DictFromPairs, ty,
                                  {xs, const_int(key_kind(kt)), const_int(own_kind(kt)),
                                   const_int(own_kind(vt)), const_int(hslot),
                                   const_int(eslot)});
            push_temp(d);
            return d;
        }
        if (c.keywords.empty()) {
            auto [hslot, eslot] = dunder_key_slots(kt);
            VReg d = call_runtime(RtFn::DictNew, ty,
                                  {const_int(key_kind(kt)), const_int(own_kind(kt)),
                                   const_int(own_kind(vt)), const_int(hslot),
                                   const_int(eslot)});
            push_temp(d);
            return d;
        }
        // dict(a=1, b=2)：关键字是字符串键。
        int vo = own_kind(vt);
        VReg d = call_runtime(RtFn::DictNew, ty,
                              {const_int(1), const_int(1), const_int(vo),
                               const_int(-1), const_int(-1)});
        push_temp(d);
        for (auto& kw : c.keywords) {
            if (kw.arg.empty()) {
                err(kw.value->line, kw.value->col, "'**' is not supported in dict()");
                continue;
            }
            VReg v = expr(*kw.value);
            v = coerce(v, tc_.type_of(kw.value.get()), vt);
            retain_elem(v, vo);
            call_void(RtFn::DictSet, {d, const_str(kw.arg), v});
        }
        return d;
    }

    if (name == "set" || name == "frozenset") {
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::Set) ty = mk_set(t_unknown());
        TypePtr et = ty->sub.empty() ? t_unknown() : ty->sub[0];
        int kk = key_kind(et), ko = own_kind(et);
        if (c.args.empty()) {
            auto [hslot, eslot] = dunder_key_slots(et);
            VReg d = call_runtime(RtFn::DictNew, ty,
                                  {const_int(kk), const_int(ko), const_int(0),
                                   const_int(hslot), const_int(eslot)});
            push_temp(d);
            return d;
        }
        if (c.args.size() != 1) {
            err(c.line, c.col, std::format("{}() takes at most one argument", name));
            return const_none();
        }
        TypePtr xt = tc_.type_of(c.args[0].get());
        if (xt && xt->kind == TyKind::Deque) {
            VReg d = expr(*c.args[0]);
            VReg xs = call_runtime(RtFn::DequeToList, mk_list(t_int()), {d});
            push_temp(xs);
            return xs;
        }
        if (!xt || (xt->kind != TyKind::List && xt->kind != TyKind::Tuple &&
                    xt->kind != TyKind::Str && xt->kind != TyKind::Bytes &&
                    xt->kind != TyKind::Dict &&
                    xt->kind != TyKind::Set && xt->kind != TyKind::File &&
                    xt->kind != TyKind::Range && xt->kind != TyKind::Generator)) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("{}() cannot iterate over '{}'", name, type_name(xt)));
            return const_none();
        }
        TypePtr se = t_unknown();
        VReg x = seq_as_list(c.args[0].get(), se);
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::SetFromList);
        auto [hslot, eslot] = dunder_key_slots(et);
        in.args = {x, const_int(kk), const_int(ko), const_int(hslot), const_int(eslot)};
        in.ty = ty;
        VReg d = def(ty, std::move(in));
        push_temp(d);
        return d;
    }

    if (name == "slice") {
        // 下标位置的形式在脱糖阶段已经折成切片语法，能走到这里说明是当
        // 独立值用——pyadamas 没有运行期的 slice 对象。
        err(c.line, c.col,
            "slice() is only supported as a subscript, e.g. xs[slice(1, 3)]");
        for (auto& a : c.args) expr(*a);
        return const_none();
    }

    if (name == "vars") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "vars() takes exactly one argument");
            return const_none();
        }
        TypePtr ot = tc_.type_of(c.args[0].get());
        if (!ot || ot->kind != TyKind::Class || !ot->cls) {
            err(c.args[0]->line, c.args[0]->col, "vars() argument must be a class instance");
            return const_none();
        }
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::Dict) ty = mk_dict(t_str(), t_unknown());
        // key 是 str（堆），value 统一 unknown：字段类型各不相同，只能保守存。
        VReg d = call_runtime(RtFn::DictNew, ty,
                              {const_int(1), const_int(1), const_int(2), const_int(-1),
                               const_int(-1)});
        push_temp(d);
        VReg obj = expr(*c.args[0]);
        for (const ClassInfo* p = ot->cls; p; p = p->base) {
            for (auto& f : p->fields) {
                int idx = ot->cls->field_index(f.name);
                if (idx < 0) continue;
                // field_load 的 imm 是字节偏移，跳过对象头的 vtable 指针。
                VReg v = retype(field_load(obj, static_cast<int>(8 * (1 + idx)), f.ty),
                                t_unknown());
                retain_elem(v, 2);
                call_void(RtFn::DictSet, {d, const_str(f.name), v});
            }
        }
        return d;
    }

    if (name == "dir") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "dir() takes at most one argument");
            return const_none();
        }
        std::vector<std::string> names;
        if (c.args.empty()) {
            for (auto& [n, s] : st_->slots) names.push_back(n);
            if (st_->is_module_top)
                for (auto& [n, gi] : global_ids_) names.push_back(n);
        } else {
            TypePtr ot = tc_.type_of(c.args[0].get());
            if (ot && ot->kind == TyKind::Class && ot->cls) {
                for (const ClassInfo* p = ot->cls; p; p = p->base) {
                    for (auto& f : p->fields) names.push_back(f.name);
                    for (auto& m : p->methods) names.push_back(m.name);
                    for (auto& a : p->class_attrs) names.push_back(a.name);
                    for (auto& [pn, pr] : p->properties) names.push_back(pn);
                }
            } else if (ot && ot->kind != TyKind::Unknown) {
                err(c.args[0]->line, c.args[0]->col,
                    std::format("dir() is not supported for '{}'", type_name(ot)));
                return const_none();
            }
            // 实参照常求值一次（可能带副作用）。
            expr(*c.args[0]);
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        TypePtr lt = tc_.type_of(&c);
        if (!lt || lt->kind != TyKind::List) lt = mk_list(t_str());
        VReg l = call_runtime(RtFn::ListNew, lt,
                              {const_int(static_cast<int64_t>(names.size())),
                               const_int(list_kind(lt))});
        push_temp(l);
        for (auto& n : names) {
            VReg s = const_str(n);
            retain_elem(s, 1);
            call_void(RtFn::ListAppend, {l, s});
        }
        return l;
    }

    if (name == "format") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "format() takes exactly two arguments");
            return const_none();
        }
        VReg v = expr(*c.args[0]);
        VReg spec = expr(*c.args[1]);
        TypePtr vt = tc_.type_of(c.args[0].get());
        RtFn fn = RtFn::FmtStr;
        if (vt) {
            switch (vt->kind) {
                case TyKind::Bool: fn = RtFn::FmtBool; break;
                case TyKind::Int: fn = RtFn::FmtI64; break;
                case TyKind::Float: fn = RtFn::FmtF64; break;
                case TyKind::Str: fn = RtFn::FmtStr; break;
                case TyKind::Complex: fn = RtFn::FmtComplex; break;
                case TyKind::Unknown: break;
                default:
                    err(c.args[0]->line, c.args[0]->col,
                        std::format("format() cannot format '{}'", type_name(vt)));
                    return const_none();
            }
        }
        VReg r = call_runtime(fn, t_str(), {v, spec});
        push_temp(r);
        return r;
    }

    if (name == "sorted" || name == "reversed") {
        if (c.args.size() != 1) {
            err(c.line, c.col, std::format("{}() takes exactly one argument", name));
            return const_none();
        }
        TypePtr xt = tc_.type_of(c.args[0].get());
        if (name == "reversed" && xt && xt->kind == TyKind::Class && xt->cls &&
            xt->cls->method("__reversed__")) {
            const MethodInfo* m = xt->cls->method("__reversed__");
            TypePtr rt = m->call_sig ? m->call_sig->ret : nullptr;
            if (m->decorated && m->decorated_ty && m->decorated_ty->kind == TyKind::Func &&
                m->decorated_ty->fn)
                rt = m->decorated_ty->fn->ret;
            VReg v = expr(*c.args[0]);
            VReg r = dunder_call(v, xt, "__reversed__", {}, nullptr);
            if (r == kNoReg) {
                err(c.line, c.col,
                    std::format("__reversed__ on '{}' must be a normal method",
                                xt->cls->name));
                return const_none();
            }
            TypePtr elem = t_unknown();
            return value_as_list(r, rt, elem);
        }
        if (xt && xt->kind == TyKind::Deque) {
            VReg d = expr(*c.args[0]);
            VReg xs = call_runtime(RtFn::DequeToList, mk_list(t_int()), {d});
            push_temp(xs);
            return xs;
        }
        if (!xt || (xt->kind != TyKind::List && xt->kind != TyKind::Tuple &&
                    xt->kind != TyKind::Str && xt->kind != TyKind::Bytes &&
                    xt->kind != TyKind::Dict &&
                    xt->kind != TyKind::Set && xt->kind != TyKind::File &&
                    xt->kind != TyKind::Range && xt->kind != TyKind::Generator)) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("{}() cannot iterate over '{}'", name, type_name(xt)));
            return const_none();
        }
        TypePtr se = t_unknown();
        VReg x = seq_as_list(c.args[0].get(), se);
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::List) ty = mk_list(se);

        // sorted(xs, key=f)：先算出每个元素的键，再按键排。
        // 之前 key= 被静默忽略——按 key 排和不按 key 排结果不同。
        Expr* key_expr = nullptr;
        for (auto& kw : c.keywords)
            if (kw.arg == "key") key_expr = kw.value.get();

        VReg l;
        if (key_expr) {
            TypePtr kt0 = tc_.type_of(key_expr);
            if (!kt0 || kt0->kind != TyKind::Func || !kt0->fn) {
                err(key_expr->line, key_expr->col,
                    "sorted() 'key' must be a function taking one argument");
                return const_none();
            }
            // 内建函数没有闭包对象，不能按用户函数那样调用。len 特判成取长度，
            // 其它内建函数明确报错——宁可报错，也不要静默排错。
            bool key_builtin = kt0->fn->unchecked;
            bool key_is_len = key_builtin && kt0->fn->name == "len";
            if (key_builtin && !key_is_len) {
                err(key_expr->line, key_expr->col,
                    std::format("sorted() 'key' does not support the builtin '{}'",
                                kt0->fn->name));
                return const_none();
            }
            if (!key_builtin && kt0->fn->pos.size() != 1 && !kt0->fn->varargs) {
                err(key_expr->line, key_expr->col,
                    "sorted() 'key' must be a function taking one argument");
                return const_none();
            }
            TypePtr kret = (kt0->fn->ret_known && kt0->fn->ret) ? kt0->fn->ret
                                                                 : t_unknown();
            TypePtr pty = kt0->fn->pos.empty() ? t_unknown() : kt0->fn->pos[0].ty;

            VReg fn = expr(*key_expr);
            VReg keys = call_runtime(RtFn::ListNew, mk_list(kret),
                                      {const_int(0), const_int(own_kind(kret))});
            push_temp(keys);

            // keys = [f(e) for e in vals]
            int idx = new_temp_slot(t_int());
            store_slot(idx, const_int(0));
            Inst ln;
            ln.op = IrOp::Len;
            ln.a = x;
            VReg n = def(t_int(), std::move(ln));
            int cond = new_block("sortkey.cond");
            int body = new_block("sortkey.body");
            int end = new_block("sortkey.end");
            Inst j;
            j.op = IrOp::Jump;
            j.target = cond;
            emit(std::move(j));
            switch_to(cond);
            Inst more;
            more.op = IrOp::Cmp;
            more.cop = CmpKind::Lt;
            more.a = load_slot(idx, t_int());
            more.b = n;
            VReg more_v = def(t_bool(), std::move(more));
            Inst br;
            br.op = IrOp::Branch;
            br.a = more_v;
            br.target = body;
            br.target2 = end;
            emit(std::move(br));
            switch_to(body);
            Inst get;
            get.op = IrOp::IndexLoad;
            get.a = x;
            get.b = load_slot(idx, t_int());
            VReg ev = def(se, std::move(get));
            ev = coerce(ev, se, pty);
            VReg kv;
            if (key_is_len) {
                // key=len：直接取长度，不走闭包调用
                Inst ln2;
                ln2.op = IrOp::Len;
                ln2.a = ev;
                kv = def(t_int(), std::move(ln2));
            } else {
                Inst call;
                call.op = IrOp::Call;
                call.a = fn;
                call.args = {ev};
                kv = def_heap(kret, std::move(call));
            }
            call_void(RtFn::ListAppend, {keys, kv});
            Inst inc;
            inc.op = IrOp::Bin;
            inc.bop = BinKind::Add;
            inc.a = load_slot(idx, t_int());
            inc.b = const_int(1);
            store_slot(idx, def(t_int(), std::move(inc)));
            {
                Inst jj;
                jj.op = IrOp::Jump;
                jj.target = cond;
                emit(std::move(jj));
            }
            switch_to(end);

            Inst in2;
            in2.op = IrOp::CallRt;
            in2.imm = static_cast<int64_t>(RtFn::ListSortedBy);
            in2.args = {x, keys, const_int(sort_kind(kret))};
            in2.ty = ty;
            l = def(ty, std::move(in2));
            push_temp(l);
        } else {
            Inst in;
            in.op = IrOp::CallRt;
            in.imm = static_cast<int64_t>(name == "sorted" ? RtFn::SortedList
                                                           : RtFn::ListReversed);
            in.args = {x};
            if (name == "sorted") in.args.push_back(const_int(sort_kind(se)));
            in.ty = ty;
            l = def(ty, std::move(in));
            push_temp(l);
        }
        // sorted(xs, reverse=True)：排完整体反转。reverse 只接受字面量，
        // 运行时的 bool 要单独做分支，这里先不做。
        if (name == "sorted") {
            for (auto& kw : c.keywords) {
                if (kw.arg == "key") continue;
                if (kw.arg != "reverse") {
                    err(kw.value->line, kw.value->col,
                        std::format("sorted() does not support the '{}' argument", kw.arg));
                    return const_none();
                }
                auto cv = as_const(kw.value.get());
                if (!cv) {
                    err(kw.value->line, kw.value->col,
                        "sorted() 'reverse' must be a literal True/False");
                    return const_none();
                }
                if (cv->i != 0) call_void(RtFn::ListReverse, {l});
            }
        }
        return l;
    }

    if (name == "enumerate") {
        if (c.args.size() != 1) {
            err(c.line, c.col, "enumerate() supports exactly one argument (no start=)");
            return const_none();
        }
        TypePtr xt = tc_.type_of(c.args[0].get());
        if (xt && xt->kind == TyKind::Deque) {
            VReg d = expr(*c.args[0]);
            VReg xs = call_runtime(RtFn::DequeToList, mk_list(t_int()), {d});
            push_temp(xs);
            return xs;
        }
        if (!xt || (xt->kind != TyKind::List && xt->kind != TyKind::Tuple &&
                    xt->kind != TyKind::Str && xt->kind != TyKind::Bytes &&
                    xt->kind != TyKind::Dict &&
                    xt->kind != TyKind::Set && xt->kind != TyKind::File &&
                    xt->kind != TyKind::Range && xt->kind != TyKind::Generator)) {
            err(c.args[0]->line, c.args[0]->col,
                std::format("enumerate() cannot iterate over '{}'", type_name(xt)));
            return const_none();
        }
        TypePtr se = t_unknown();
        VReg x = seq_as_list(c.args[0].get(), se);
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::List) ty = mk_list(mk_tuple({t_int(), se}));
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::EnumerateList);
        in.args = {x};
        in.ty = ty;
        VReg l = def(ty, std::move(in));
        push_temp(l);
        return l;
    }

    if (name == "zip") {
        if (c.args.size() != 2) {
            err(c.line, c.col, "zip() supports exactly two iterables for now");
            return const_none();
        }
        TypePtr se1 = t_unknown(), se2 = t_unknown();
        VReg a = seq_as_list(c.args[0].get(), se1);
        VReg b = seq_as_list(c.args[1].get(), se2);
        TypePtr ty = tc_.type_of(&c);
        if (!ty || ty->kind != TyKind::List) ty = mk_list(mk_tuple({se1, se2}));
        Inst in;
        in.op = IrOp::CallRt;
        in.imm = static_cast<int64_t>(RtFn::Zip2List);
        in.args = {a, b};
        in.ty = ty;
        VReg l = def(ty, std::move(in));
        push_temp(l);
        return l;
    }

    if (name == "bytes") {
        if (c.args.size() > 1) {
            err(c.line, c.col, "bytes() expects at most 1 argument");
            return const_none();
        }
        TypePtr ty = t_bytes();
        if (c.args.empty()) return retype(const_str(""), ty);
        TypePtr at = tc_.type_of(c.args[0].get());
        VReg v = expr(*c.args[0]);
        if (at && at->kind == TyKind::Bytes) return v;
        if (at && at->kind == TyKind::Int) {
            VReg b = call_runtime(RtFn::BytesZeros, ty, {v});
            push_temp(b);
            return b;
        }
        if (at && at->kind == TyKind::List) {
            VReg b = call_runtime(RtFn::BytesFromList, ty, {v});
            push_temp(b);
            return b;
        }
        err(c.args[0]->line, c.args[0]->col,
            std::format("bytes() argument must be int, bytes or list[int], got '{}'",
                        type_name(at)));
        return const_none();
    }

    err(c.line, c.col, std::format("builtin '{}' is not supported by the code generator yet", name));
    return const_none();
}

}  // namespace pyadamas
