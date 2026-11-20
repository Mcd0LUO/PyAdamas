// block_bitset.h —— 块级位集（支配集/活跃性分析共用）
//
// 动机：opt_loop 的支配集与 codegen_regalloc 的块级活跃性，原实现都是
// 「每块一个 nv / nb 字节的 char 向量」。大函数上（实测 234_re_opt.py
// 单函数 626 块 × 2150 vreg）单趟扫描就是 MB 级字节流量，不动点迭代还要
// 乘轮数，成为编译吞吐的头号热点（perf：compute_dom 43%、
// assign_vreg_slots 18%）。
//
// 这里只把「同一组 0/1 用什么容器存」换成 64 位字位集：判定逻辑、交集
// 顺序、不动点迭代次数全部保持原样，因此结果逐位不变（生成 asm 逐字节
// 不变是硬闸门）。末字多余位在每次写入后清零，所以逐字相等 == 逐位相等。
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace pyadamas {

class BlockBits {
  public:
    BlockBits() = default;
    BlockBits(size_t nbits, bool fill)
        : nbits_(nbits), w_((nbits + 63u) / 64u, fill ? ~0ULL : 0ULL) {
        mask_tail();
    }

    void reset(size_t nbits, bool fill = false) {
        nbits_ = nbits;
        w_.assign((nbits + 63u) / 64u, fill ? ~0ULL : 0ULL);
        mask_tail();
    }

    size_t nbits() const { return nbits_; }
    size_t word_count() const { return w_.size(); }
    uint64_t* words() { return w_.data(); }
    const uint64_t* words() const { return w_.data(); }

    bool test(size_t i) const {
        return i < nbits_ && ((w_[i >> 6] >> (i & 63u)) & 1ULL) != 0;
    }
    void set(size_t i) {
        if (i < nbits_) w_[i >> 6] |= 1ULL << (i & 63u);
    }
    void clear(size_t i) {
        if (i < nbits_) w_[i >> 6] &= ~(1ULL << (i & 63u));
    }

    void assign(const BlockBits& o) {
        nbits_ = o.nbits_;
        w_ = o.w_;
    }
    // this |= o（两侧宽度必须一致）
    void or_with(const BlockBits& o) {
        for (size_t i = 0; i < w_.size(); ++i) w_[i] |= o.w_[i];
    }
    // this = (src & ~clr) | orv —— 块级活跃性 in = (out & ~def) | use
    void andnot_or(const BlockBits& src, const BlockBits& clr, const BlockBits& orv) {
        for (size_t i = 0; i < w_.size(); ++i)
            w_[i] = (src.w_[i] & ~clr.w_[i]) | orv.w_[i];
    }

    bool operator==(const BlockBits& o) const { return w_ == o.w_; }

    // 按位下标升序遍历置位（原逐字节扫描语义相同；活跃区间的 min/max
    // 更新与遍历顺序无关，但保持升序便于对照原实现）。
    template <typename F>
    void for_each_set(F&& f) const {
        for (size_t wi = 0; wi < w_.size(); ++wi) {
            uint64_t x = w_[wi];
            while (x != 0) {
                const unsigned b = static_cast<unsigned>(__builtin_ctzll(x));
                f(static_cast<size_t>(wi) * 64u + b);
                x &= x - 1;
            }
        }
    }

  private:
    void mask_tail() {
        if (w_.empty()) return;
        const size_t tail = nbits_ & 63u;
        if (tail != 0) w_.back() &= (~0ULL >> (64u - tail));
    }

    size_t nbits_ = 0;
    std::vector<uint64_t> w_;
};

}  // namespace pyadamas
