// codegen_comment.cpp —— 汇编注释发射
//
// 每条指令都带一条 IR 文本注释（"\t# %12 = bin.add %10, %11"）。注释是
// 生成 asm 的一部分（逐字节对照的基准），不能省；能省的是「拼文本的临时
// 分配」：三个入口都复用 emit_.line_buf，一条注释只留 MItem 自己那一次分配。
// 原来 comment() 是 "\t# " + s 造临时串、comment(in.text()) 再多一次
// in.text() 的临时串，每条指令 3 次分配。
#include "codegen_impl.h"

namespace pyadamas {

void AsmGen::comment(std::string_view s) {
    emit_.line_buf.clear();
    emit_.line_buf += "\t# ";
    emit_.line_buf.append(s);
    emit_.out.push(make_comment(emit_.line_buf));
}

void AsmGen::comment_inst(const Inst& in) {
    emit_.line_buf.clear();
    emit_.line_buf += "\t# ";
    in.text_into(emit_.line_buf);
    emit_.out.push(make_comment(emit_.line_buf));
}

void AsmGen::comment0(std::string_view s) {
    emit_.line_buf.clear();
    emit_.line_buf += "# ";
    emit_.line_buf.append(s);
    emit_.out.push(make_comment(emit_.line_buf));
}

}  // namespace pyadamas
