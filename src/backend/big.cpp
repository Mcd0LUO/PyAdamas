// backend/big.cpp —— P3 big 的代码生成（字面量 / 运算 / 转换）
//
// 新逻辑集中在新文件（域隔离）。big 运算在降级期直接发 CallRt
// （pa_big_*，与 Complex 同形态），这里只负责字面量发射；静态 BigInt
// 对象的 limb 在编译期由十进制串算好（与运行期 rt_big.c 的表示逐位一致）。
#include "backend/impl.h"

namespace pyadamas {

// 十进制串（可带负号）→ 基 2^32 小端 limb。返回带符号 limb 数
// （0 = 零，负 = 负数），与 PyAdamasBig.nd 同口径。编译期专用，
// 不依赖运行时（编译器不能调 rt.o）。
int64_t big_literal_limbs(const std::string& dec, std::vector<uint32_t>& out) {
    out.clear();
    if (dec.empty()) return 0;
    size_t i = 0;
    bool neg = false;
    if (dec[0] == '-' || dec[0] == '+') { neg = dec[0] == '-'; i = 1; }
    // Horner：acc = acc*10 + digit（uint64 中间量，逐 limb 进位）。
    for (; i < dec.size(); ++i) {
        char c = dec[i];
        if (c < '0' || c > '9') continue;   // 词法已保证合法，防御性跳过
        uint64_t carry = static_cast<uint64_t>(c - '0');
        for (size_t k = 0; k < out.size(); ++k) {
            uint64_t cur = static_cast<uint64_t>(out[k]) * 10 + carry;
            out[k] = static_cast<uint32_t>(cur);
            carry = cur >> 32;
        }
        while (carry) {
            out.push_back(static_cast<uint32_t>(carry));
            carry >>= 32;
        }
    }
    while (!out.empty() && out.back() == 0) out.pop_back();
    int64_t n = static_cast<int64_t>(out.size());
    return neg ? -n : n;
}

// const.big：永生静态对象地址（.LbigN，数据段在 backend/codegen.cpp 发射）。
// 与 const.str 同口径：借用、不入临时栈；BIG tag 由 static_tag_of 在
// 消费点（unknown 实参/存储）导出，这里只在 dst 确有 tag 字时补写。
void AsmGen::gen_const_big(const Inst& in) {
    char b[24];
    auto rr = std::to_chars(b, b + sizeof b, in.idx.value);
    std::string sym = ".Lbig";
    sym.append(b, rr.ptr);
    emit2(MOp::kLeaq, op_mem_sym(std::move(sym), "%rip"), kRax);
    move_to(dst_mop(in.dst), mreg_operand(kRax));
    emit_tag_imm(in.dst, kTagBig);
}

}  // namespace pyadamas
