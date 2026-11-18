// Replay type checking when a call consumed a provisional return type.
#include "typecheck.h"

namespace pyadamas {
namespace {

// Replay uses a fresh module and scope graph. Carry only structural value
// types across rounds; class/struct layouts and callable signatures belong
// to the checker that built them.
bool replayable(const TypePtr& ty, int depth = 0) {
    if (!ty || depth > 12 || ty->celem != CKind::None) return false;
    switch (ty->kind) {
        case TyKind::Unknown: return ty == t_unknown();
        case TyKind::None:
        case TyKind::Bool:
        case TyKind::Int:
        case TyKind::Float:
        case TyKind::Complex:
        case TyKind::Str:
        case TyKind::Bytes: return true;
        case TyKind::List:
        case TyKind::Set:
        case TyKind::Dict:
        case TyKind::Tuple:
        case TyKind::Optional:
            return std::ranges::all_of(ty->sub, [&](const auto& sub) {
                return replayable(sub, depth + 1);
            });
        default: return false;
    }
}

TypePtr snapshot(const TypePtr& ty) {
    if (ty->sub.empty()) return ty;
    auto copy = std::make_shared<Type>(*ty);
    for (auto& sub : copy->sub) sub = snapshot(sub);
    return copy;
}

}  // namespace

void TypeChecker::check(Module& m) {
    check_once(m);
    size_t rounds_left = func_defs_.size() + 16;
    while (collect_result_hints()) {
        if (rounds_left-- == 0) {
            err(1, 1, "call result type inference did not converge; add return annotations");
            break;
        }
        // Do not re-run statements on a partially mutated scope graph. In
        // particular, assignments, imports, classes and narrowing must all
        // see their original source order, and provisional diagnostics must
        // not leak into the final pass.
        TypeChecker next(file_);
        next.module_lookup_ = module_lookup_;
        next.lenient_imports_ = lenient_imports_;
        next.strict_module_names_ = strict_module_names_;
        next.cinclude_dirs_ = cinclude_dirs_;
        next.result_hints_ = std::move(result_hints_);
        next.check_once(m);
        *this = std::move(next);
    }
}

TypePtr TypeChecker::observe_call_result(Call& c, const FuncInfoPtr& fn, TypePtr result) {
    auto* sig = fn->outer ? fn->outer : fn.get();
    auto def = signature_defs_.find(sig);
    if (def == signature_defs_.end() || sig->is_async || sig->is_generator) {
        observed_results_.erase(&c);
        return result;
    }
    if (auto it = result_hints_.find(&c);
        it != result_hints_.end() && it->second.def == def->second) {
        result = snapshot(it->second.ty);
    }
    // Track the type consumed now, not a mutable container that callers may
    // subsequently widen or an annotation whose holes may be filled later.
    observed_results_.insert_or_assign(&c, CallResult{def->second,
                                      replayable(result) ? snapshot(result) : result});
    return result;
}

bool TypeChecker::collect_result_hints() {
    bool changed = false;
    decltype(result_hints_) next;
    for (auto& [call, observed] : observed_results_) {
        auto it = func_infos_.find(observed.def);
        if (it == func_infos_.end() || !it->second->ret_known || it->second->is_generator) continue;
        TypePtr result = it->second->ret ? it->second->ret : t_none();
        if (!replayable(result)) {
            // If an earlier round supplied a hint that is no longer sound,
            // remove it and recheck instead of keeping the stale value type.
            if (auto old = result_hints_.find(call);
                old != result_hints_.end() && old->second.def == observed.def) changed = true;
            continue;
        }
        if (!type_eq(observed.ty, result)) changed = true;
        next.emplace(call, CallResult{observed.def, snapshot(result)});
    }
    if (changed) result_hints_ = std::move(next);
    return changed;
}

}  // namespace pyadamas
