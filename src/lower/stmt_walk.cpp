// lower/stmt_walk.cpp —— 语句树深度优先遍历（try/with 形状判定用）
//
// 不进入嵌套 FunctionDef 的函数体（那是另一个函数）；但进入 ClassDef 的类体
// ——类体语句是在当前函数帧里降级的，它的 try/with/return 都算在本函数头上。
#include "lower/impl.h"

namespace pyadamas {

void walk_stmt_tree(std::vector<StmtPtr>& body, const std::function<void(Stmt&)>& pre) {
    for (auto& sp : body) {
        if (!sp) continue;
        Stmt& s = *sp;
        pre(s);
        switch (s.kind) {
            case StmtKind::FunctionDef:
                break;
            case StmtKind::Try: {
                auto& t = static_cast<Try&>(s);
                walk_stmt_tree(t.body, pre);
                for (auto& h : t.handlers) walk_stmt_tree(h.body, pre);
                walk_stmt_tree(t.orelse, pre);
                walk_stmt_tree(t.finalbody, pre);
                break;
            }
            case StmtKind::With:
                walk_stmt_tree(static_cast<With&>(s).body, pre);
                break;
            case StmtKind::If: {
                auto& i = static_cast<If&>(s);
                for (auto& b : i.branches) walk_stmt_tree(b.body, pre);
                walk_stmt_tree(i.orelse, pre);
                break;
            }
            case StmtKind::While: {
                auto& w = static_cast<While&>(s);
                walk_stmt_tree(w.body, pre);
                walk_stmt_tree(w.orelse, pre);
                break;
            }
            case StmtKind::For: {
                auto& f = static_cast<For&>(s);
                walk_stmt_tree(f.body, pre);
                walk_stmt_tree(f.orelse, pre);
                break;
            }
            case StmtKind::Match: {
                auto& m = static_cast<Match&>(s);
                for (auto& c : m.cases) walk_stmt_tree(c.body, pre);
                break;
            }
            case StmtKind::ClassDef:
                walk_stmt_tree(static_cast<ClassDef&>(s).body, pre);
                break;
            default:
                break;
        }
    }
}

}  // namespace pyadamas
