// vect_planner.h —— 向量化器协作对象（阶段 3 第一块，逐方法搬）
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "emit_sink.h"
#include "ir.h"
#include "mach_instr.h"
#include "slot_query.h"
#include "value_query.h"
#include "vect_rules.h"

namespace pyadamas {

// AsmGen 只在这里前向声明：Loop 持有它的引用，方法体定义在 vect_planner.cpp
// （那里已 include codegen_impl.h，AsmGen 完整）。
class AsmGen;

struct AffIdx {
    bool ok = false;
    int64_t coef = 1;
    VReg inv = kNoReg;
    int64_t off = 0;
};
constexpr int64_t kAffMaxCoef = int64_t{1} << 24;
constexpr int64_t kAffMaxOff = int64_t{1} << 24;
struct AffBase {
    std::string key;
    VReg inv = kNoReg;
    MOperand inv_src;
    MReg reg = kMRegNone;
    int64_t coef = 1;
    int64_t off_max = 0;
    int64_t off_min = 0;
};
struct AffEq {
    VReg x = kNoReg;
    VReg y = kNoReg;
    MOperand xm;
    MOperand ym;
};

// 向量值分类（try_vect_loop 的分析结果）。原为 AsmGen 的内嵌类型，阶段 3
// 第 6 步随常量归并方法搬到 VectPlanner：codegen_vect_jvec.cpp 的寄存器规划
// （同值常量归并 / 归纳变量生命周期 / 临时值线性扫描）要读同一份分类，判据
// 只有一处。codegen_vect.h 里留别名，类内 VClass 写法不变。
// InvF/InvI 是循环不变量（setup 广播）；J 是归纳变量；Affine 是 j+常量
//（只用于下标位移）；VecF/VecI 是逐 lane 计算的 2-lane 值；Acc 是
// reduction 累加器旧值；Fail 不可向量化。
// 分类枚举与判据的唯一出处在 vect_rules.h（具名 + 编译期真值表）。
struct VClass {
    vect::VK kind = vect::VK::Fail;
    int64_t off = 0;          // Affine：j+off；Convert j_src：[j+off, j+off+1]
    VReg list = kNoReg;       // VecF/VecI 来自 IndexLoad 时的 list vreg
    bool is_load = false;     // VecF/VecI 是内存装载（vs 算术）
    VReg mov_src = kNoReg;    // Mov 透明别名 / 同值常量归并的规范 vreg
    bool j_src = false;       // Convert 输入是归纳变量 j（或 j+常量）
};

// 槽的循环内写/读统计（原 try_vect_loop 的局部 struct，阶段 A4 第 2 步搬到
// 这里：Loop::si 字段要被多个 Loop 方法读）。
struct SlotInfo {
    int writers = 0;
    int store_pos = -1;
    VReg store_val = kNoReg;
    bool read_before_store = false;
    int loads = 0;
    bool outside_read = false;
    int min_load_pos = 1 << 30;
};

// 体块一条指令在向量循环里的角色（阶段 A4 第 1 步从 AsmGen 类体搬来：
// Loop 要按它发射，codegen_vect.h 里留别名，类内写法不变）。
enum class VRole : uint8_t { Skip, Load, Bin, MapStore, RedStore, Fail };

// 一条下标访问的仿射描述（coef*j + inv + off）。
struct VAccess {
    VReg list = kNoReg;
    int64_t off = 0;
    int64_t coef = 1;
    VReg inv = kNoReg;
    bool write = false;
    int pos = 0;              // 体块内指令序号（读在写前判定）
    int bidx = -1;            // 基址键下标（AffBase）
};

// 一个 reduction 累加器（int 走 4-lane 时 IACC 当 ymm 用）。
struct VRed {
    int slot = -1;
    VReg acc = kNoReg, expr = kNoReg, add = kNoReg;
    int store_pos = -1;
    bool is_float = false;
    MOperand loc{};                 // 槽位置
    bool acc_stack = false;         // float acc 在栈（scratch+每轮写回）
    MReg XT = kMRegNone;            // float acc 寄存器
    MReg IACC = kMRegNone;          // int acc 寄存器
};

// 4-lane 发射上下文：全部是 try_vect_loop 已定死的分析/分配结果（只读）。
struct VAvx2Ctx {
    const std::vector<Inst>* body = nullptr;
    const std::vector<VRole>* role = nullptr;
    const std::vector<VAccess>* acc = nullptr;
    const std::vector<int>* acc_at = nullptr;
    const std::vector<AffBase>* bases = nullptr;
    const std::vector<VRed>* reds = nullptr;
    const std::function<MReg(VReg)>* xmm_of = nullptr;
    const std::vector<VReg>* uniq_inv = nullptr;
    const std::unordered_map<VReg, MReg>* inv_reg = nullptr;
    const std::vector<int64_t>* kcoefs = nullptr;
    const std::unordered_map<int64_t, MReg>* kreg = nullptr;
    MReg jreg = kMRegNone;
    bool j_stack = false;
    MOperand jloc;
    MOperand n_op;
    MReg gscr = kMRegNone;    // coef>1 装载的打包 scratch（VPINSRQ 用）
    MReg seq1 = kMRegNone;    // int 乘 pmuludq 重建 scratch
    MReg seq2 = kMRegNone;
    MReg hscr = kMRegNone;    // int reduction 水平求和 scratch（折叠也用）
    // 归纳变量物化寄存器（P5b，2-lane 侧由 vect_jvec2 物化 [j, j+1]）：
    // 4-lane 循环自己把它扩成 [j, j+1, j+2, j+3]（低 128 位 + vpinsrq
    // 拼高 128 位）。jvec == kMRegNone → 体块里没有 j 参与算术。
    MReg jvec = kMRegNone;
    bool has_j = false;
    const std::string* vec = nullptr;   // SSE2 2-lane 循环标签（回退目标）
    bool int_ok = false;      // 体块全是 int 值（无任何 VecF/InvF/float acc）
};

class VectPlanner {
public:
    // 一次 try_vect_loop 调用的全部局部状态 + 原 21 个捕获 lambda 变成的
    // 成员方法（阶段 A4 第 1/2 步）。来源：src/codegen_vect.cpp 的
    // try_vect_loop；方法体在 vect_planner.cpp。
    struct Loop {
        // 一次调用的上下文：owner_ 提供 VectPlanner 的接口，gen_ 提供 AsmGen
        // 的私有分析/发射辅助（AsmGen 里 friend struct VectPlanner::Loop），BI
        // 是体块指令表。构造于 try_vect_loop 入口。
        Loop(VectPlanner& owner, AsmGen& gen, const std::vector<Inst>& insts)
            : owner_(owner), gen_(gen), BI(insts) {
            // 4-lane 发射器（codegen_vect_avx2.cpp）经 VAvx2Ctx 的
            // std::function 回调查寄存器；xmm_of 现在是方法，这里存绑定 this
            // 的适配器（跨文件接口 VAvx2Ctx 保持不变）。
            xmm_of_fn = [this](VReg v) { return xmm_of(v); };
        }
        // 方法体里沿用的类型别名（与 codegen_vect.cpp 的局部别名同义）。
        using Red = VRed;
        using Access = VAccess;
        using Role = VRole;
        using VK = vect::VK;

        // ── 循环结构 ──
        int body_bi = -1;         // 体块块号（cur_block_）
        int incr_bi = -1;         // 增量块块号
        int cond_bi = -1;         // 条件块块号
        int sJ = -1;              // 归纳变量槽
        int end_bi = -1;          // cond 分支目标块（simd_incr_cond_ok 出参）
        int jupd_pos = -1;        // while 形状：体块尾部增量的起始指令位置
        VReg n = kNoReg;          // 循环上界 vreg
        MOperand n_op;            // 上界的入口可读操作数
        bool failed = false;      // 角色判定失败标志

        // ── 槽 / 表达式 / 访问 / 归约（原被多个 lambda 共享，A4 第 2 步搬来）──
        std::unordered_map<int, SlotInfo> si;   // 槽 → 循环内读写统计
        std::vector<int> jalias;                // j-alias 槽
        std::vector<VRed> reds;                 // reduction 累加器
        std::vector<int> temp_slots;            // 体局部临时槽
        std::unordered_map<VReg, VClass> cls;   // 表达式 DAG 分类
        std::vector<VAccess> accesses;          // 下标访问描述
        std::vector<int> acc_at;                // 体块位置 → accesses 下标
        std::vector<VReg> inv_floats;           // 需要广播的不变量 float
        std::vector<VReg> inv_ints;             // 需要广播的不变量 int
        bool has_int_mul = false;               // int 乘 → pmuludq 重建需序列 scratch
        bool has_convert = false;               // int→float Convert → cvtdq2pd 折算
        bool has_intdiv = false;                // int // 2^m → 算术右移合成
        bool has_j_operand = false;             // 归纳变量 j 参与整数算术
        std::unordered_set<VReg> visiting;      // classify 递归环检测
        std::vector<MReg> xmm_pool{kXmm0, kXmm1, kXmm2, kXmm3,
                                   kXmm4, kXmm5, kXmm6, kXmm7};
        std::unordered_map<VReg, MReg> inv_reg; // 不变量广播 → xmm
        MReg JVEC = kMRegNone;                  // 归纳变量物化寄存器
        std::unordered_map<VReg, MReg> tmp_reg; // 临时值 → xmm
        std::function<MReg(VReg)> xmm_of_fn;    // xmm_of 的跨文件适配器（见构造）

        // ── 分析结果（角色 / 逻辑数组 / 守卫 / 基址）──
        std::unordered_map<VReg, char> nonindex_use;   // 非下标使用点（P5b）
        std::vector<VRole> role;                       // 体块指令角色
        std::unordered_map<std::string, std::vector<int>> by_list;  // 数组键 → 访问下标
        std::unordered_map<std::string, VReg> list_rep;             // 数组键 → 代表 vreg
        std::vector<std::string> lists;                // 数组键（确定性顺序）
        std::vector<AffEq> inv_eq;                     // 入口 inv 判等守卫
        std::vector<AffBase> bases;                    // 基址键
        std::vector<int64_t> kcoefs;                   // coef>1 的跨步计数器系数

        // ── 寄存器分配（dry-run）──
        bool j_stack = false;         // 归纳变量在栈（否则在 JREG）
        MReg JREG = kMRegNone;        // 归纳变量工作寄存器
        std::vector<MReg> gpr_pool{kRdi, kRdx, kRcx};
        std::size_t gpr_used = 0;
        std::unordered_map<int64_t, MReg> kreg;        // coef → 跨步计数器寄存器
        bool any_int_red = false;
        MReg SEQ1 = kMRegNone, SEQ2 = kMRegNone;
        MReg TWO32 = kMRegNone;
        MReg GSCR = kMRegNone;
        std::vector<VReg> uniq_inv;                    // 去重后的不变量广播清单
        std::unordered_map<VReg, int> last_use;        // 临时值末次使用位置
        int j_first = -1, j_last = -1;                 // 归纳变量算术使用区间
        MReg FSCR = kMRegNone;
        MReg HSCR = kMRegNone;
        // math.sqrt（CallRt MathSqrt）的 2-lane 发射：SQRZ = 循环前清零的 0.0
        // （负数守卫比较用），SQRTMP = 打包高 lane 的临时。has_sqrt 由 classify 置位。
        bool has_sqrt = false;
        MReg SQRZ = kMRegNone;
        MReg SQRTMP = kMRegNone;

        // ── 发射 ──
        std::string tail;             // 标量尾标签
        std::string vec;              // 向量循环标签
        bool avx_int = true;          // 体块纯整数 → 可走 4-lane
        VAvx2Ctx avx;                 // AVX2 发射上下文
        std::string vexit;            // 两条循环的公共出口标签
        bool avx2 = false;            // 是否发了 AVX2 段

        // ── 方法（阶段 A4 第 2 步：原捕获 lambda）──
        bool in_loop(int bi) const;
        bool slot_is_jalias(int slot) const;
        bool slot_is_red(int slot) const;
        Red* red_of_slot(int slot);
        bool is_red_add(VReg v) const;
        bool slot_is_temp(int slot) const;
        VReg temp_store_val(int slot) const;
        MReg take(MReg prefer);
        VReg resolve_src(VReg v) const;

        bool is_jalias_load(VReg v) const;
        int64_t index_off(VReg v) const;
        bool cross_block(VReg v) const;
        void pshufd(int64_t imm, MReg r) const;
        std::string list_key(VReg lv) const;
        MOperand list_base_mop(VReg lv) const;
        bool list_base_ok(VReg lv);
        bool inv_int_ok(VReg v);
        // inv_int_ok 作为 std::function 回调传给 vect_affine_index 的适配器。
        std::function<bool(VReg)> inv_int_pred();
        int list_elem_float(VReg lv);

        VK classify(VReg v);
        // vreg → 发射期 xmm（不变量广播 / 归纳变量物化 / 临时值）。
        MReg xmm_of(VReg v) const;

        // ── 上下文引用（构造参数；声明在最后，初始化顺序与构造列表一致）──
        VectPlanner& owner_;
        AsmGen& gen_;
        const std::vector<Inst>& BI;
    };

    VectPlanner(EmitSink& e, ValueQuery& v, SlotQuery& s, AsmGen& g)
        : emit_(e), val_(v), slot_(s), gen_(g) {}

    // ── 通用循环向量化入口（阶段 A4 第 3 步：从 AsmGen::try_vect_loop 搬来）──
    // 识别可数 for + 逐元素 map/reduction，匹配成功发「守卫 + SSE2 向量循环 +
    // jmp cond」，返回 true；不匹配返回 false，一个字节不发。实现见
    // vect_planner_loop.cpp（AsmGen::try_vect_loop 留一行转发）。
    bool try_loop(const Block& b);

    // 除数是不是正的 2 的幂常量；是则写回移位量 sh。
    bool vect_div_pow2(VReg b, int* sh) const;
    // // 与 %（除数为 2 的幂常量）的 2-lane packed 发射：rd 已持有被除数，原地写回。
    void vect_intdiv2(MReg rd, MReg tmp, bool is_mod, int sh);

    // ── 仿射下标的发射（阶段 3 第 3 步：纯发射，无捕获状态）──────────
    // K = coef*8*j：进循环前算一次，循环里每轮 += coef*16。
    void vect_affine_kinit(MReg k, MReg jreg, int64_t coef);
    void vect_affine_step(MReg k, int64_t coef);
    // coef==1 用 movupd 整装；coef>1 逐 lane movsd + unpcklpd（SSE2 无 gather）。
    void vect_affine_load(MReg base, MReg j, MReg k, int64_t coef, int64_t off,
                          MReg dst, MReg scratch);
    // 写出只支持 coef==1（scatter 在角色判定处就拒绝）。
    void vect_affine_store(MReg base, MReg j, int64_t off, MReg src);

    // ── 归纳变量物化（阶段 3 第 4 步）──
    // dst = [j+off, j+off+1]：与 Convert 的 j_src 分支逐字节同一序列。
    void vect_jvec2(MReg jreg, int64_t off, MReg dst, MReg scr);

    // ── 仿射下标解析（阶段 3 第 6 步：纯分析，无捕获状态）──
    // 递归解析下标表达式；depth 仅防病态深链。成功 = 可折进寻址的仿射式。
    AffIdx vect_affine_index(VReg v, const std::vector<int>& jalias,
                             const std::function<bool(VReg)>& inv_ok,
                             int depth = 0) const;
    // 规划期校验（发射前）：常量编不进 imm32/disp32 → 整段保守跳过。
    bool vect_affine_plan_ok(const std::vector<AffBase>& bases) const;

    // ── 同值常量归并（阶段 3 第 6 步）──
    // 按值把重复的 InvI/InvF 常量 vreg 接到首个 vreg 的 mov_src 上（广播
    // 寄存器只占一个）。语义等价，只为省寄存器。
    void vect_merge_const_invs(std::unordered_map<VReg, VClass>* cls);

    // ── 仿射守卫与基址（阶段 3 第 5 步）──
    bool vect_affine_guard(const MOperand& list_obj, const AffBase& b,
                           const MOperand& n_op, const std::string& tail);
    void vect_affine_base(const MOperand& list_obj, const AffBase& b, MReg reg);
    void vect_affine_inv_eq(const AffEq& eq, const std::string& tail);

private:
    EmitSink& emit_;
    ValueQuery& val_;
    SlotQuery& slot_;
    // try_loop 里原样保留的 AsmGen 私有辅助/成员访问走这个引用（friend）。
    AsmGen& gen_;
};

}  // namespace pyadamas
