// backend/codegen.cpp —— IR → x86-64 汇编：数据段、函数遍历与总入口
#include "backend/impl.h"
#include "backend/codegen.h"
#include "backend/eh_emit.h"
#include "ir/module.h"  // fnv1a64：类元数据的稳定 cid（0.531）

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace pyadamas {

static std::string debug_display_name(const std::string& name0) {
    std::string name = name0;
    auto dollar = name.find('$');
    if (dollar != std::string::npos) name = name.substr(0, dollar);
    auto pos = name.rfind('_');
    if (pos != std::string::npos && pos > 0 && pos + 1 < name.size()) {
        bool digits = true;
        for (size_t i = pos + 1; i < name.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(name[i]))) { digits = false; break; }
        if (digits) return name.substr(0, pos);
    }
    return name;
}

// 与 runtime/rt_dict.c 的字符串哈希保持一致：FNV-1a 64 再 mix64。
// 长串字面量把结果直接写进 PyAdamasStr::hash，字典查字面量键时就不用再算，
// 也不用在 .data.rel.ro（RELRO 后被保护为只读）上做懒缓存。
static uint64_t str_literal_hash(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebull;
    h ^= h >> 31;
    return h ? h : 1;
}

// 数据段直写助手：to_chars + 复用缓冲，免每行 std::format 临时串。
// buf 用 AsmGen::emit_.line_buf（数据段发射不与 line() 嵌套）。
static void quad_dec(MachFunc& out, std::string& buf, int64_t v) {
    buf = "\t.quad ";
    char b[24];
    auto r = std::to_chars(b, b + sizeof b, v);
    buf.append(b, r.ptr);
    buf += '\n';
    out += buf;
}
static void quad_hex(MachFunc& out, std::string& buf, uint64_t v) {
    buf = "\t.quad 0x";
    char b[24];
    auto r = std::to_chars(b, b + sizeof b, v, 16);
    buf.append(b, r.ptr);
    buf += '\n';
    out += buf;
}
static void quad_sym(MachFunc& out, std::string& buf, std::string_view s) {
    buf = "\t.quad ";
    buf += s;
    buf += '\n';
    out += buf;
}
static void append_num(std::string& buf, int64_t v) {
    char t[24];
    auto r = std::to_chars(t, t + sizeof t, v);
    buf.append(t, r.ptr);
}
static void raw_label2(MachFunc& out, std::string& buf, const char* prefix, int64_t a, int64_t b) {
    buf = prefix;
    append_num(buf, a);
    buf += '_';
    append_num(buf, b);
    buf += ":\n";
    out += buf;
}
static void quad_sym2(MachFunc& out, std::string& buf, const char* prefix, int64_t a, int64_t b) {
    buf = "\t.quad ";
    buf += prefix;
    append_num(buf, a);
    buf += '_';
    append_num(buf, b);
    buf += '\n';
    out += buf;
}
static void raw_label(MachFunc& out, std::string& buf, std::string_view name) {
    buf = name;
    buf += ":\n";
    out += buf;
}

// 回调跳板做参数提升时用的寄存器名（SysV 整数参数寄存器）。
static const char* const kCbReg64[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};
static const char* const kCbReg32[6] = {"%edi", "%esi", "%edx", "%ecx", "%r8d", "%r9d"};
static const char* const kCbReg8[6] = {"%dil", "%sil", "%dl", "%cl", "%r8b", "%r9b"};
static const char* const kCbReg16[6] = {"%di", "%si", "%dx", "%cx", "%r8w", "%r9w"};
// 统一调用约定的整型实参寄存器（与 kCbReg64 相同序列，语义不同：
// 这里表示「第 i 个实参进第 i 个整型寄存器」，与 C ABI 的浮点槽无关。
static const char* const kCbArgReg[6] = {"%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};

std::string AsmGen::gen(const IRModule& m) {
        m_ = &m;
        opt_ = m.opt_level;
        emit_.out.clear();
        tag_.eh_plans.clear();
        eh_plan_active_ = false;
        dbg_on_ = !m.source_path.empty();
        dbg_entries_.clear();
        dbg_seq_ = 0;
        bool has_del_class = false;  // 0.539：有 __del__ 的类存在时给运行期一个强符号
        global_labels_.resize(m.globals.size());
        for (size_t i = 0; i < m.globals.size(); ++i)
            global_labels_[i] = m.globals[i].symbol.empty()
                                    ? ".Lg" + std::to_string(i)
                                    : m.globals[i].symbol;
        global_mops_.resize(m.globals.size());
        global_tag_mops_.resize(m.globals.size());
        for (size_t i = 0; i < m.globals.size(); ++i) {
            global_mops_[i] = op_mem_sym(global_labels_[i], "%rip");
            global_tag_mops_[i] = op_mem_sym(global_labels_[i] + "$t", "%rip");
        }

        // P3 门控（设计 §3.4）：模块含 unknown 动态算术/比较才启用 shadow
        // tag。26 个热基准全部不含 pa_any_* → 门控关 → 不进任何 tag 分支。
        // 降级器仍发旧 AnyArith/AnyCmp/AnyNeg，gen_call_rt 在门控内拦截
        // 改发 tag 版（pa_any_*_t）；门控关时逐字旧路径。
        tag_.tags_enabled = false;
        for (const auto& fp : m.funcs)
            for (const auto& b : fp->blocks)
                for (const auto& in : b.insts)
                    if (in.op == IrOp::CallRt &&
                        (in.imm == static_cast<int64_t>(RtFn::AnyArith) ||
                         in.imm == static_cast<int64_t>(RtFn::AnyCmp) ||
                         in.imm == static_cast<int64_t>(RtFn::AnyNeg) ||
                         (gnu_cfi() && (in.imm == static_cast<int64_t>(RtFn::AnyBigBinary) ||
                                        in.imm == static_cast<int64_t>(RtFn::AnyInvert)))))
                        tag_.tags_enabled = true;
        // P2（0.340，PYADAMAS_TYPE_TAGS）：运行期类型标签需要 tag 区与传播
        // 在所有模块都生效（pprint/isinstance 靠它跨调用边界带类型），
        // 所以门控直接并上 tag_.type_tags。tag_.type_tags 关时这一行是纯或 0，
        // 既有 P3 门控与 asm 逐字节不变。
        if (tag_.type_tags) tag_.tags_enabled = true;

        if (!m.strings.empty()) {
            // 字符串对象里有一个指向自身字符区的绝对地址。放在 .rodata
            // 会让 PIE 链接器报 "relocation in read-only section"，
            // 必须用可重定位的只读段。
            section_relro();
            for (size_t i = 0; i < m.strings.size(); ++i) {
                const std::string& s = m.strings[i].text;
                emit_.out += "\t.align 8\n";
                // 隐藏 RC 头（64 字节）：next/prev/rc/dtor/release/size/kind/marked。
                // 字面量 rc = INT64_MIN 永生；启动时只登记进 RC 索引供
                // unknown 类型检查使用，不进 GC 堆链。
                emit_.out += "\t.quad 0\n\t.quad 0\n";
                emit_.out += "\t.quad 0x8000000000000000\n";
                emit_.out += "\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n";
                raw_label(emit_.out, emit_.line_buf, num_label(".Lstr", static_cast<int64_t>(i)));
                // +0 pub：str 是码点数、bytes 是字节数；编译期就算好（RELRO 只读，
                // 运行期不能懒缓存）。+8 字节数，+16 hash（SSO 字符区），
                // +24 data，+32 chash（字典哈希缓存）。
                //
                // **chash 在编译期算好。** 字面量落在 .data.rel.ro，RELRO 之后
                // 是只读的，运行期没法懒缓存 —— 所以必须在这里填。
                // 这样字典查字面量键（if k == "abc" 之类）完全不用算哈希。
                quad_dec(emit_.out, emit_.line_buf, m.strings[i].pub);
                quad_dec(emit_.out, emit_.line_buf, static_cast<int64_t>(s.size()));
                if (s.size() > 7)
                    quad_hex(emit_.out, emit_.line_buf, str_literal_hash(s));
                else
                    emit_.out += "\t.quad 0\n";
                quad_sym(emit_.out, emit_.line_buf, num_label(".Lstr", static_cast<int64_t>(i)) + "_c");
                quad_hex(emit_.out, emit_.line_buf, str_literal_hash(s));
                raw_label(emit_.out, emit_.line_buf, num_label(".Lstr", static_cast<int64_t>(i)) + "_c");
                emit_.line_buf = "\t.ascii \"";
                emit_.line_buf += asm_escape(s);
                emit_.line_buf += "\"\n";
                emit_.out += emit_.line_buf;
                emit_.out += "\t.byte 0\n";
            }
            // 静态字面量虽然永生，但 unknown 值的运行期类型检查要靠 RC
            // 注册表确认载荷类型。表在模块入口统一登记。
        }
        // P3 big 字面量：永生 BigInt 静态对象。RC 头与 rt_big.c 的
        // PyAdamasBig 布局逐位一致（size=8+4n，kind=9=BIGINT）；limb 在
        // 编译期由十进制串算好。无 big 的程序这一段不发射（asm 闸门）。
        if (!m.bigs.empty()) {
            section_relro();
            for (size_t i = 0; i < m.bigs.size(); ++i) {
                std::vector<uint32_t> limbs;
                int64_t nd = big_literal_limbs(m.bigs[i], limbs);
                emit_.out += "\t.align 8\n";
                emit_.out += "\t.quad 0\n\t.quad 0\n";               // next/prev
                emit_.out += "\t.quad 0x8000000000000000\n";           // rc 永生
                emit_.out += "\t.quad 0\n\t.quad 0\n";               // dtor/release
                quad_dec(emit_.out, emit_.line_buf, 8 + static_cast<int64_t>(limbs.size()) * 4);
                quad_dec(emit_.out, emit_.line_buf, 9);                       // kind BIGINT
                emit_.out += "\t.quad 0\n";                            // marked
                raw_label(emit_.out, emit_.line_buf, num_label(".Lbig", static_cast<int64_t>(i)));
                quad_dec(emit_.out, emit_.line_buf, nd);
                for (uint32_t l : limbs)
                    emit_.out += std::format("\t.long {}\n", l);
            }
        }
        section_relro();
        emit_.out += "\t.align 8\n.Lstrtab:\n";
        for (size_t i = 0; i < m.strings.size(); ++i)
            quad_sym(emit_.out, emit_.line_buf, num_label(".Lstr", static_cast<int64_t>(i)));
        // big 字面量与 str 字面量共用登记表（pa_register_literals 只按
        // RC 头登记索引，载荷类型由 kind 区分）。
        for (size_t i = 0; i < m.bigs.size(); ++i)
            quad_sym(emit_.out, emit_.line_buf, num_label(".Lbig", static_cast<int64_t>(i)));
        if (m.strings.empty() && m.bigs.empty()) emit_.out += "\t.quad 0\n";

        if (!m.globals.empty()) {
            emit_.out += "\t.bss\n";
            emit_.out += "\t.align 8\n";
            for (size_t i = 0; i < m.globals.size(); ++i) {
                if (m.globals[i].external) continue;
                if (!m.globals[i].symbol.empty())
                    emit_.out += "\t.globl " + m.globals[i].symbol + "\n";
                emit_.line_buf = global_label(static_cast<int>(i));
                emit_.line_buf += ":\n\t.zero 8\n";
                emit_.out += emit_.line_buf;
                // P3：unknown 全局的平行 tag 字（仅门控内、且别的模块写不到
                // 的全局，见 global_tag_eligible）。初值 0 = UNK 探针回退。
                if (tag_.tags_enabled && global_tag_eligible(static_cast<int>(i))) {
                    if (!m.globals[i].symbol.empty())
                        emit_.out += "\t.globl " + global_tag_label(static_cast<int>(i)) + "\n";
                    emit_.line_buf = global_tag_label(static_cast<int>(i));
                    emit_.line_buf += ":\n\t.zero 8\n";
                    emit_.out += emit_.line_buf;
                }
            }

            // GC 根表：所有本模块全局槽的地址；外部全局的存储属于对方模块。
            section_relro();
            emit_.out += "\t.align 8\n";
            raw_label(emit_.out, emit_.line_buf, roots_label());
            for (size_t i = 0; i < m.globals.size(); ++i) {
                if (m.globals[i].external) continue;
                quad_sym(emit_.out, emit_.line_buf, global_label(static_cast<int>(i)));
            }
        } else {
            section_relro();
            emit_.line_buf = "\t.align 8\n";
            emit_.line_buf += roots_label();
            emit_.line_buf += ":\n";
            emit_.out += emit_.line_buf;
        }

        // 非入口模块的惰性初始化标志。
        if (!m.is_entry && !m.module_symbol.empty()) {
            emit_.out += "\t.bss\n\t.align 8\n";
            emit_.out += std::format("\t.globl {}\n{}:\n\t.zero 8\n",
                                module_ready_symbol(m.module_symbol),
                                module_ready_symbol(m.module_symbol));
        }

        // 每个类一张静态 vtable：槽里是方法代码地址。对象头第一个
        // 字段指向它，虚调用就是「取表、取槽、间接跳转」。
        // vtable 前固定 7 个字的元数据：MRO 元数据指针表、类编号、
        // vtable 自身、本类声明的槽表、模块限定类名、动态属性槽偏移、
        // __eq__ 槽号（-1 = 无实现；unknown 域相等用，见 rt_any.c）。
        if (!m.classes.empty()) {
            section_relro();
            // 同名类重定义时接口导出的是最后一次绑定，符号也只导出最后一份。
            std::unordered_map<std::string, size_t> last_cls;
            for (size_t i = 0; i < m.classes.size(); ++i) last_cls[m.classes[i].name] = i;
            for (size_t i = 0; i < m.classes.size(); ++i) {
                const IRClass& c = m.classes[i];
                // 导入祖先的 mirror 元数据：cid 固定 -1，只用于限定名比较，
                // vtable/own_slots 指向定义模块导出的表。
                for (size_t j = 0; j < c.ext_bases.size(); ++j) {
                    const IRClassExtBase& e = c.ext_bases[j];
                    emit_.out += "\t.align 8\n";
                    raw_label2(emit_.out, emit_.line_buf, ".Lvtmetaext", static_cast<int64_t>(i), static_cast<int64_t>(j));
                    emit_.out += "\t.quad 0\n";
                    emit_.out += "\t.quad -1\n";
                    quad_sym(emit_.out, emit_.line_buf, e.vtable_symbol);
                    quad_sym(emit_.out, emit_.line_buf, e.own_slots_symbol);
                    quad_sym2(emit_.out, emit_.line_buf, ".Lcnameext", static_cast<int64_t>(i), static_cast<int64_t>(j));
                    emit_.out += "\t.quad 0\n";  // 导入类没有动态属性槽
                    emit_.out += "\t.quad 0\n";  // 导入类没有 dunder 槽表（实例 metadata 走定义模块那份）
                    // 第 8 个字：跨模块稳定的类 id（限定名的 FNV-1a 64，0.531）——
                    // 定义模块与导入方各算一次，天然一致。
                    emit_.out += std::format("\t.quad {}\n",
                                             static_cast<long long>(fnv1a64(e.qualname)));
                    raw_label2(emit_.out, emit_.line_buf, ".Lcnameext", static_cast<int64_t>(i), static_cast<int64_t>(j));
                    emit_.line_buf = "\t.asciz \"";
                    emit_.line_buf += asm_escape(e.qualname);
                    emit_.line_buf += "\"\n";
                    emit_.out += emit_.line_buf;
                }
                emit_.out += "\t.align 8\n";
                raw_label(emit_.out, emit_.line_buf, num_label(".Lvtmeta", static_cast<int64_t>(i)));
                quad_sym(emit_.out, emit_.line_buf, num_label(".Lvtmro", static_cast<int64_t>(i)));
                quad_dec(emit_.out, emit_.line_buf, static_cast<int64_t>(i));
                quad_sym(emit_.out, emit_.line_buf, num_label(".Lvt", static_cast<int64_t>(i)));
                quad_sym(emit_.out, emit_.line_buf, num_label(".Lvtown", static_cast<int64_t>(i)));
                quad_sym(emit_.out, emit_.line_buf, num_label(".Lcname", static_cast<int64_t>(i)));
                // 第 6 个字：动态属性槽偏移（0 = 没有）；第 7 个字：dunder 槽表指针。
                quad_dec(emit_.out, emit_.line_buf, c.dict_off);
                quad_sym(emit_.out, emit_.line_buf, num_label(".Lvtdun", static_cast<int64_t>(i)));
                // 第 8 个字：跨模块稳定的类 id（限定名的 FNV-1a 64，0.531）。
                // 与导入方同一个口径：`<模块运行时名>.<类名>`（导入侧用
                // origin_qualname / 「模块名.类名」构造，两边必须逐字节一致）。
                emit_.out += std::format("\t.quad {}\n",
                                         static_cast<long long>(fnv1a64(
                                             m.runtime_name + "." + c.name)));
                bool export_cls = !m.is_entry && !m.module_symbol.empty() &&
                                  !c.name.empty() && last_cls[c.name] == i;
                raw_label(emit_.out, emit_.line_buf, num_label(".Lvt", static_cast<int64_t>(i)));
                if (export_cls) {
                    std::string sym = module_class_vtable_symbol(m.module_symbol, c.name);
                    emit_.out += std::format("\t.globl {}\n\t.hidden {}\n{}:\n", sym, sym, sym);
                }
                if (c.vtable.empty()) emit_.out += "\t.quad 0\n";
                for (size_t s = 0; s < c.vtable.size(); ++s) {
                    const std::string& ext =
                        s < c.vtable_ext.size() ? c.vtable_ext[s] : std::string();
                    if (!ext.empty()) {
                        quad_sym(emit_.out, emit_.line_buf, ext);
                    } else if (c.vtable[s] < 0) {
                        emit_.out += "\t.quad 0\n";
                    } else {
                        quad_sym(emit_.out, emit_.line_buf, entry_ref(c.vtable[s]));
                    }
                }
                // 每个已填槽导出裸代码符号：导入方 vtable 需要函数地址，
                // 不能引用 .LvtN + offset（那是数据槽地址）。
                if (export_cls) {
                    for (size_t s = 0; s < c.vtable.size(); ++s) {
                        std::string impl;
                        const std::string& ext =
                            s < c.vtable_ext.size() ? c.vtable_ext[s] : std::string();
                        if (!ext.empty())
                            impl = ext;
                        else if (c.vtable[s] >= 0)
                            impl = entry_ref(c.vtable[s]);
                        if (impl.empty()) continue;
                        std::string sym =
                            module_class_method_symbol(m.module_symbol, c.name,
                                                       static_cast<int>(s));
                        emit_.out += std::format("\t.globl {}\n\t.hidden {}\n\t.set {}, {}\n",
                                            sym, sym, sym, impl);
                    }
                }
                raw_label(emit_.out, emit_.line_buf, num_label(".Lvtmro", static_cast<int64_t>(i)));
                quad_dec(emit_.out, emit_.line_buf, static_cast<int64_t>(c.mro.size()));
                for (int a : c.mro) {
                    if (a >= 0)
                        quad_sym(emit_.out, emit_.line_buf, num_label(".Lvtmeta", a));
                    else
                        quad_sym2(emit_.out, emit_.line_buf, ".Lvtmetaext", static_cast<int64_t>(i), -a - 1);
                }
                raw_label(emit_.out, emit_.line_buf, num_label(".Lvtown", static_cast<int64_t>(i)));
                if (export_cls) {
                    std::string sym = module_class_own_slots_symbol(m.module_symbol, c.name);
                    emit_.out += std::format("\t.globl {}\n\t.hidden {}\n{}:\n", sym, sym, sym);
                }
                quad_dec(emit_.out, emit_.line_buf, static_cast<int64_t>(c.own_slots.size()));
                for (int s : c.own_slots)
                    quad_dec(emit_.out, emit_.line_buf, s);
                if (c.del_slot >= 0) has_del_class = true;
                // dunder 槽表：[__eq__, __lt__, __hash__, __del__, __str__, __repr__]；
                // unknown 域据此分派（rt_class.c 的 pa_instance_dunder_fn 按下标取）。
                raw_label(emit_.out, emit_.line_buf, num_label(".Lvtdun", static_cast<int64_t>(i)));
                quad_dec(emit_.out, emit_.line_buf, c.eq_slot);
                quad_dec(emit_.out, emit_.line_buf, c.lt_slot);
                quad_dec(emit_.out, emit_.line_buf, c.hash_slot);
                quad_dec(emit_.out, emit_.line_buf, c.del_slot);   // 0.539: __del__（-1 = 无）
                quad_dec(emit_.out, emit_.line_buf, c.str_slot);   // BUG-3: __str__（-1 = 无）
                quad_dec(emit_.out, emit_.line_buf, c.repr_slot);  // BUG-3: __repr__（-1 = 无）
                raw_label(emit_.out, emit_.line_buf, num_label(".Lcname", static_cast<int64_t>(i)));
                emit_.line_buf = "\t.asciz \"";
                emit_.line_buf += asm_escape(c.qualname);
                emit_.line_buf += "\"\n";
                emit_.out += emit_.line_buf;
            }
        }

        // 每个函数一个静态闭包（code + 0 个捕获），用作无捕获函数值。
        // 布局和堆闭包一致：+0 code，+8 n_caps，+16 起是 cell。
        if (!m.funcs.empty()) {
            section_relro();
            for (size_t i = 0; i < m.funcs.size(); ++i) {
                emit_.out += "\t.align 8\n";
                emit_.out += "\t.quad 0\n\t.quad 0\n";
                emit_.out += "\t.quad 0x8000000000000000\n";
                emit_.out += "\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n\t.quad 0\n";
                raw_label(emit_.out, emit_.line_buf, num_label(".Lclos", static_cast<int64_t>(i)));
                // 间接调用一律走统一约定：+0 指向 shim（签名含浮点的函数），
                // 否则就是函数本体（统一约定与它的 ABI 恰好一致）。
                if (closure_needs_shim(*m.funcs[i]))
                    quad_sym(emit_.out, emit_.line_buf, shim_label(static_cast<int>(i)));
                else
                    quad_sym(emit_.out, emit_.line_buf, entry_ref(static_cast<int>(i)));
                emit_.out += "\t.quad 0\n";
            }
        }

        // 0.539：程序里只要有类实现了 __del__，就给运行时一个强符号覆盖弱定义
        // （rt_del.c 的 pa_has_del_classes），释放路径据此短路；没有 __del__ 的
        // 程序只多一次全局读，实测 binary_trees 不退化。
        if (has_del_class) {
            emit_.out += "\t.data\n\t.align 8\n\t.globl pa_has_del_classes\n";
            emit_.out += "pa_has_del_classes:\n\t.quad 1\n";
        }

        if (dbg_on_) {
            // 运行时 traceback 表引用的源路径与函数名。
            section_rodata();
            emit_.out += std::format("\t.Ldbfile: .asciz \"{}\"\n", asm_escape(m.source_path));
            for (size_t i = 0; i < m.funcs.size(); ++i) {
                emit_.line_buf = "\t.Lfnname";
                append_num(emit_.line_buf, static_cast<int64_t>(i));
                emit_.line_buf += ": .asciz \"";
                // P4 块3：lowerer 填了显示名就用它（门控开：<module>/裸方法名），
                // 否则旧口径（门控关恒空，逐字节不变）。
                emit_.line_buf += asm_escape(m.funcs[i]->debug_name.empty()
                                            ? debug_display_name(m.funcs[i]->name)
                                            : m.funcs[i]->debug_name);
                emit_.line_buf += "\"\n";
                emit_.out += emit_.line_buf;
            }
        }
        emit_.out += "\t.text\n";
        // 手写跳板（类析构器 / GC 释放 / 生成器帧释放）的实参寄存器与
        // 影子空间也走 ABI：它们由 C 运行时按平台调用约定回调，最内层
        // 实参寄存器在 Win64 是 %rcx 而不是 %rdi。
        const std::string tramp_a0 = arg_reg_i(0);
        const std::string tramp_a1 = arg_reg_i(1);
        const int tramp_shadow = target_abi().shadow_space;
        // DWARF 行号：.file 1 指定源路径，函数体内的 .loc 指向行号。
        if (!m.source_path.empty())
            emit_.out += std::format("\t.file 1 \"{}\"\n", asm_escape(m.source_path));

        // 每个有堆字段的类生成一个析构器，对象 rc 归零时由运行时回调。
        // 字段按 field_heap 处理：1 = 已知堆指针（直接 dec），2 = unknown
        // （可能装着整数或堆指针，走注册表判断）。字段偏移与布局一致。
        for (size_t i = 0; i < m.classes.size(); ++i) {
            const IRClass& c = m.classes[i];
            bool has_heap = std::ranges::any_of(c.field_heap, [](uint8_t v) { return v != 0; });
            // 有动态属性槽的类即使没有堆字段也要有析构器：槽里的 dict 归它释放。
            if (!has_heap && c.dict_off == 0) continue;
            emit_.out += "\n# ---- dtor class " + c.name + " ----\n";
            emit_.out += "\t.align 8\n";
            emit_.out += std::format(".Ldtor{}:\n", i);
            if (gnu_cfi()) emit_.out += "\t.cfi_startproc\n";
            emit_.out += "\tpushq %rbx\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_offset %rbx, -16\n";
            emit_.out += "\tmovq " + tramp_a0 + ", %rbx\n";
            if (tramp_shadow) emit_.out += std::format("\tsubq ${}, %rsp\n", tramp_shadow);
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                emit_.out += std::format("\tmovq {}(%rbx), {}\n", 8 * (f + 1), tramp_a0);
                emit_.out += c.field_heap[f] == 2 ? "\tcall pa_rc_dec_unknown\n"
                                             : "\tcall pa_rc_dec\n";
            }
            if (c.dict_off != 0) {
                // 动态属性槽：可能还没分配过（NULL），跳过。
                emit_.out += std::format("\tmovq {}(%rbx), {}\n", c.dict_off, tramp_a0);
                emit_.out += std::format("\ttestq {}, {}\n\tje 1f\n\tcall pa_rc_dec\n1:\n",
                                    tramp_a0, tramp_a0);
            }
            if (tramp_shadow) emit_.out += std::format("\taddq ${}, %rsp\n", tramp_shadow);
            emit_.out += "\tpopq %rbx\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 8\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_restore %rbx\n";
            emit_.out += "\tret\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_endproc\n";

            // GC 清扫用：只把指向可达对象的字段引用减掉，不释放任何东西。
            emit_.out += "\n# ---- gc release class " + c.name + " ----\n";
            emit_.out += "\t.align 8\n";
            emit_.out += std::format(".Lgcrel{}:\n", i);
            if (gnu_cfi()) emit_.out += "\t.cfi_startproc\n";
            emit_.out += "\tpushq %rbx\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_offset %rbx, -16\n";
            emit_.out += "\tmovq " + tramp_a0 + ", %rbx\n";
            if (tramp_shadow) emit_.out += std::format("\tsubq ${}, %rsp\n", tramp_shadow);
            for (size_t f = 0; f < c.field_heap.size(); ++f) {
                if (!c.field_heap[f]) continue;
                emit_.out += std::format("\tmovq {}(%rbx), {}\n", 8 * (f + 1), tramp_a0);
                emit_.out += c.field_heap[f] == 2 ? "\tcall pa_rc_dec_unknown\n"
                                             : "\tcall pa_gc_unref_child\n";
            }
            if (c.dict_off != 0) {
                emit_.out += std::format("\tmovq {}(%rbx), {}\n", c.dict_off, tramp_a0);
                emit_.out += std::format("\ttestq {}, {}\n\tje 1f\n\tcall pa_gc_unref_child\n1:\n",
                                    tramp_a0, tramp_a0);
            }
            if (tramp_shadow) emit_.out += std::format("\taddq ${}, %rsp\n", tramp_shadow);
            emit_.out += "\tpopq %rbx\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 8\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_restore %rbx\n";
            emit_.out += "\tret\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_endproc\n";
        }

        // 生成器帧的槽释放（GC release 与异常逃出共用）：逐槽 dec 并清零。
        // 清零让函数可以重复调用：正常结束时 status==1、槽已释放，直接跳过；
        // 异常逃出后由运行时调用一次，之后 GC 清扫再调到也是空操作。
        // GC 清扫期间 pa_rc_dec 自带「只调可达对象计数」的保护。
        for (size_t i = 0; i < m.generators.size(); ++i) {
            const IRGenerator& g = m.generators[i];
            emit_.out += std::format("\n# ---- gcrel gen g{} ----\n", i);
            emit_.out += "\t.align 8\n";
            emit_.out += std::format(".Lgengcrel{}:\n", i);
            if (gnu_cfi()) emit_.out += "\t.cfi_startproc\n";
            emit_.out += "\tpushq %rbx\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_offset %rbx, -16\n";
            emit_.out += "\tmovq " + tramp_a0 + ", %rbx\n";
            if (tramp_shadow) emit_.out += std::format("\tsubq ${}, %rsp\n", tramp_shadow);
            // **异常上下文槽必须在 status 检查之前无条件释放。**
            //
            // status==1 表示生成器已正常终止，RC 槽在 gen_finish 里就释放过
            // 了，所以下面整块跳过。但 exc_cur_slot / exc_top_slot 不同：
            // gen_finish 的顺序是 release_slots() 然后 exc_leave() ——
            // exc_leave 把 current_exc 写回槽，那一刻槽里才有值，而
            // release_slots 已经跑完了。于是正常终止的生成器，帧槽里会
            // 留下一个异常对象（或 save 链），而 status==1 又把释放整块跳过，
            // 对象永远不还。
            //
            // 实测（PYADAMAS_EXC_TRACE + PYADAMAS_GRELDBG，119）：
            //   [exc#5] leave  KeyError at 0x...2a8     ← 写进 s1
            //   [grel]  g=0x...250 status=1 s1=0x...070  ← 销毁时 s1 仍有值
            //   [exc#5] LEAK   KeyError                  ← 没人还
            for (size_t s = 0; s < g.slot_heap.size(); ++s) {
                if (g.slot_heap[s] < 3) continue;
                const int off = 8 * (kGenHeader + static_cast<int>(s));
                emit_.out += std::format("\tleaq {}(%rbx), {}\n", off, tramp_a0);
                emit_.out += std::format("\tmovq ${}, {}\n",
                                    static_cast<int>(g.slot_heap[s]), tramp_a1);
                emit_.out += "\tcall pa_gen_slot_release\n";
            }
            emit_.out += "\tcmpq $1, 8(%rbx)\n";
            emit_.out += std::format("\tje .Lgengcreldone{}\n", i);
            for (size_t s = 0; s < g.slot_heap.size(); ++s) {
                if (!g.slot_heap[s] || g.slot_heap[s] >= 3) continue;
                const int off = 8 * (kGenHeader + static_cast<int>(s));
                // slot_heap: 1=堆，2=可能装着非堆值（unknown 流过堆槽）。
                // 清零只在 GC 清扫阶段做；trial/restore 阶段运行时只调计数，
                // 不能破坏生成器帧的内容。
                emit_.out += std::format("\tleaq {}(%rbx), {}\n", off, tramp_a0);
                emit_.out += std::format("\tmovq ${}, {}\n",
                                    g.slot_heap[s] == 2 ? 2 : 1, tramp_a1);
                emit_.out += "\tcall pa_gen_slot_release\n";
            }
            emit_.out += std::format(".Lgengcreldone{}:\n", i);
            if (tramp_shadow) emit_.out += std::format("\taddq ${}, %rsp\n", tramp_shadow);
            emit_.out += "\tpopq %rbx\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 8\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_restore %rbx\n";
            emit_.out += "\tret\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_endproc\n";
        }

        for (size_t i = 0; i < m.funcs.size(); ++i) emit_function(*m.funcs[i], static_cast<int>(i));

        // 统一调用约定的 shim：按真实签名把整型寄存器里的实参重排到
        // %xmm / 栈，返回值也按真实签名放回 %rax。间接调用点不需要知道签名。
        for (size_t i = 0; i < m.funcs.size(); ++i)
            if (closure_needs_shim(*m.funcs[i]))
                emit_closure_shim(*m.funcs[i], static_cast<int>(i));

        if (dbg_on_ && !dbg_entries_.empty()) {
            section_relro(); emit_.out += "\t.align 8\n";
            emit_.out += ".Ldbgtab:\n\t.quad .Ldbfile\n\t.quad .Ldbg_entries\n";
            emit_.out += std::format("\t.quad {}\n.Ldbg_entries:\n", dbg_entries_.size());
            for (const auto& e : dbg_entries_) {
                emit_.out += std::format("\t.quad {}\n", e.label);
                if (e.fi >= 0)
                    emit_.out += std::format("\t.quad {}\n", dbg_name_label(e.fi));
                else
                    emit_.out += "\t.quad 0\n";
                emit_.out += std::format("\t.long {}\n\t.long 0\n", e.line);
            }
            emit_.out += "\t.text\n";
        }

        // C 回调跳板：C 只传函数地址，不传环境，所以调用点先把 PyAdamas 闭包
        // 登记到每个签名专属的槽里；跳板把闭包放进 %r10（PyAdamas 的 env 约定）
        // 后直接跳到函数体。32 位/布尔参数按声明宽度提升到 64 位。
        if (!m.cb_sigs.empty()) {
            emit_.out += "\n# ---- callback trampolines ----\n";
            for (size_t i = 0; i < m.cb_sigs.size(); ++i) {
                emit_.out += std::format("\t.align 8\n.Lcbtramp{}:\n", i);
                // 跳板自成一段 CFI 区域。入口 CFA=rsp+8；下发 call 前会
                // subq $8 补对齐，CFA 相应变成 rsp+16，返回前恢复。
                if (gnu_cfi()) emit_.out += "\t.cfi_startproc\n";
                emit_.out += std::format("\tmovq .Lcbslot{}(%rip), %r10\n", i);
                int gi = 0;
                for (CKind k : m.cb_sigs[i].params) {
                    if (k == CKind::Float) continue;  // xmm 参数直接透传
                    if (k == CKind::Int32)
                        emit_.out += std::format("\tmovslq {}, {}\n", kCbReg32[gi], kCbReg64[gi]);
                    else if (k == CKind::UInt32)
                        emit_.out += std::format("\tmovl {}, {}\n", kCbReg32[gi], kCbReg32[gi]);
                    else if (k == CKind::Bool || k == CKind::UInt8)
                        emit_.out += std::format("\tmovzbl {}, {}\n", kCbReg8[gi], kCbReg32[gi]);
                    else if (k == CKind::Int8)
                        emit_.out += std::format("\tmovsbq {}, {}\n", kCbReg8[gi], kCbReg64[gi]);
                    else if (k == CKind::Int16)
                        emit_.out += std::format("\tmovswq {}, {}\n", kCbReg16[gi], kCbReg64[gi]);
                    else if (k == CKind::UInt16)
                        emit_.out += std::format("\tmovzwq {}, {}\n", kCbReg16[gi], kCbReg64[gi]);
                    ++gi;
                }
                // C 回调走真实 C ABI（浮点在 %xmm），而闭包 +0 指向统一约定入口。
                // 这里把 C 侧实参按位置搬到统一约定的整型寄存器序列，再进 shim；
                // 浮点返回值由 shim 放 %rax，这里搬回 %xmm0 满足 C 侧。
                int ci = 0;
                for (CKind k : m.cb_sigs[i].params) {
                    if (k == CKind::Float)
                        emit_.out += std::format("\tmovq %xmm{}, {}\n", ci, kCbArgReg[ci]);
                    else
                        emit_.out += std::format("\tmovq {}, {}\n", kCbReg64[ci], kCbArgReg[ci]);
                    ++ci;
                }
                emit_.out += "\txorl %eax, %eax\n";
                // **必须先补 8 字节再 call。**
                //
                // 跳板由 C 当普通函数调用：入口 rsp ≡ 8 (mod 16)。它自己要
                // 再发一次 call，按 ABI 要求 call 时 rsp ≡ 0，所以要先 subq $8。
                //
                // 漏掉这 8 字节时，被调方（PyAdamas 函数或 closure shim）在
                // misaligned 栈上执行；gcc 15 的运行时函数用 movaps 溢出 xmm
                // 直接 #GP。实测：112_callbacks.py、113_qsort.py 在 gcc 15 上
                // Illegal instruction。
                emit_.out += "\tsubq $8, %rsp\n";
                if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 16\n";
                emit_.out += "\tcall *(%r10)\n";
                emit_.out += "\taddq $8, %rsp\n";
                if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 8\n";
                if (m.cb_sigs[i].ret == CKind::Float)
                    emit_.out += "\tmovq %rax, %xmm0\n";
                emit_.out += "\tret\n";
                if (gnu_cfi()) emit_.out += "\t.cfi_endproc\n";
            }
            emit_.out += "\t.bss\n\t.align 8\n";
            for (size_t i = 0; i < m.cb_sigs.size(); ++i)
                emit_.out += std::format(".Lcbslot{}:\n\t.zero 8\n", i);
            emit_.out += "\t.text\n";
        }

        // 免写直调（from c import / dylib 句柄调用）：符号名 C 字符串 +
        // 默认句柄的解析缓存槽。名字表按符号去重，槽在首次调用时填。
        if (!m.c_lazy_syms.empty()) {
            section_relro();
            for (size_t i = 0; i < m.c_lazy_syms.size(); ++i)
                emit_.out += std::format(".Lcsymstr{}:\n\t.asciz \"{}\"\n", i,
                                    asm_escape(m.c_lazy_syms[i]));
            emit_.out += "\t.bss\n\t.align 8\n";
            for (size_t i = 0; i < m.c_lazy_syms.size(); ++i)
                emit_.out += std::format(".Lcsym{}:\n\t.zero 8\n", i);
            emit_.out += "\t.text\n";
        }

        if (!m.is_entry && !m.module_symbol.empty() && m.entry >= 0) {
            emit_.out += "\t.globl " + module_init_symbol(m.module_symbol) + "\n";
            emit_type_func(module_init_symbol(m.module_symbol));
            emit_.out += module_init_symbol(m.module_symbol) + ":\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_startproc\n";
            emit_.out += "\tpushq %rbp\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_offset %rbp, -16\n";
            emit_.out += "\tmovq %rsp, %rbp\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_register %rbp\n";
            emit_.out += std::format("\tleaq .Lstrtab(%rip), {}\n", arg_reg_i(0));
            emit_.out += std::format("\tmovq ${}, {}\n", m.strings.size() + m.bigs.size(), arg_reg_i(1));
            emit_.out += "\tcall pa_register_literals\n";
            // 单字节 str 字面量在生成代码里直接引用运行时驻留表
            // pa_char_tab（见 gen_inst 的 ConstStr），所以函数体之前必须
            // 保证这张表已填好。放这里是因为它依赖 pa_disable_unknown_index
            // 的结果（驻留对象要不要登记进 RC 索引）。
            emit_.out += "\tcall pa_static_strs_init\n";
            if (dbg_on_ && !dbg_entries_.empty())
            emit_.out += std::format("\tleaq .Ldbgtab(%rip), {}\n\tcall pa_debug_register\n",
                                    arg_reg_i(0));
            emit_.out += "\tpopq %rbp\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa %rsp, 8\n";
            // 尾跳进 __pa_main：返回地址原样留在栈顶，CFA 规则跟刚进
            // 本函数时一致，展开器在这里看到的仍是调用方的帧。
            emit_.out += "\tjmp " + entry_ref(m.entry) + "\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_endproc\n";
        }

        // 真正的入口：调用降级出来的 __pa_main，返回它的返回值
        if (m.is_entry) {
            emit_.out += "\t.globl main\n";
            emit_type_func("main");
            emit_.out += "main:\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_startproc\n";
            emit_.out += "\tpushq %rbp\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_offset 16\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_offset %rbp, -16\n";
            emit_.out += "\tmovq %rsp, %rbp\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa_register %rbp\n";
            emit_.out += std::format("\tsubq ${}, %rsp\n", out_base() + 16);
            emit_.out += std::format("\tmovq {}, (%rsp)\n", arg_reg_i(0));
            emit_.out += std::format("\tmovq {}, 8(%rsp)\n", arg_reg_i(1));
            // GC 根表 + 栈顶。栈顶取 main 的 rbp，收集器向下扫描。
            emit_.out += std::format("\tleaq {}(%rip), {}\n", roots_label(), arg_reg_i(0));
            emit_.out += std::format("\tmovq ${}, {}\n", local_global_count(), arg_reg_i(1));
            emit_.out += std::format("\tmovq %rbp, {}\n", arg_reg_i(2));
            emit_.out += "\tcall pa_gc_init\n";
            emit_.out += "\tcall pa_rc_stats_init\n";
            if (disable_unknown_index_)
                emit_.out += "\tcall pa_disable_unknown_index\n";
            else if (index_readonly_)
                emit_.out += "\tcall pa_set_index_readonly\n";
            emit_.out += std::format("\tleaq .Lstrtab(%rip), {}\n", arg_reg_i(0));
            emit_.out += std::format("\tmovq ${}, {}\n", m.strings.size() + m.bigs.size(), arg_reg_i(1));
            emit_.out += "\tcall pa_register_literals\n";
            emit_.out += "\tcall pa_static_strs_init\n";
            if (dbg_on_ && !dbg_entries_.empty())
            emit_.out += std::format("\tleaq .Ldbgtab(%rip), {}\n\tcall pa_debug_register\n",
                                    arg_reg_i(0));
            emit_.out += std::format("\tmovslq (%rsp), {}\n", arg_reg_i(0));
            emit_.out += std::format("\tmovq 8(%rsp), {}\n", arg_reg_i(1));
            emit_.out += "\tcall pa_sys_set_args\n";
            emit_.out += std::format("\taddq ${}, %rsp\n", out_base() + 16);
            if (m.entry >= 0) emit_.out += std::format("\tcall {}\n", entry_ref(m.entry));
            emit_.out += "\tpopq %rbp\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_def_cfa %rsp, 8\n";
            emit_.out += "\tret\n";
            if (gnu_cfi()) emit_.out += "\t.cfi_endproc\n";
            emit_size_func("main");
        }
        // 零开销异常（Z2-b2）：所有函数的 .gcc_except_table + 每个 TU 一份的
        // DW.ref 间接指针槽。空计划（老路径）一个字节都不发。
        emit_eh_tables();
        emit_gnu_stack_note();

        if (std::getenv("PYADAMAS_TIMING"))
            std::fprintf(stderr, "[timing] codegen alloc=%.1f emit=%.1f ms\n",
                         alloc_ns_ / 1e6, emit_ns_ / 1e6);
        return render(emit_.out);
    }

// ── 零开销异常（Z2-b2，--zero-cost-exc）────────────────────────────────

void AsmGen::prepare_eh_plan(const IRFunction& f, const std::string& fname) {
    tag_.eh_begin_labels.clear();
    tag_.eh_end_labels.clear();
    tag_.eh_lp_labels.clear();
    tag_.eh_begin_seq = tag_.eh_end_seq = tag_.eh_lp_seq = 0;
    eh_plan_active_ = false;
    if (!zero_cost_exc_ || f.eh_regions.empty() || !gnu_cfi()) return;
    // 预扫描顺序 = 块发射顺序 × 块内指令顺序；发射标记指令时按同样的顺序
    // 取标签，一一对应。
    for (const auto& b : f.blocks)
        for (const auto& in : b.insts) {
            if (in.op == IrOp::EhBegin) tag_.eh_begin_labels.push_back(fresh_label("ehB"));
            else if (in.op == IrOp::EhEnd) tag_.eh_end_labels.push_back(fresh_label("ehE"));
            else if (in.op == IrOp::EhLandingPad)
                tag_.eh_lp_labels.push_back(fresh_label("ehLP"));
        }
    // 每个 EhBegin/EhEnd/EhLandingPad 三元组构成一条保护区（嵌套 try 有多条）。
    // 它们共用同一份 LSDA（多条 call-site 记录）与同一个 landing pad 分发入口。
    const size_t n_regions = tag_.eh_begin_labels.size();
    if (n_regions == 0 || tag_.eh_end_labels.size() != n_regions ||
        tag_.eh_lp_labels.size() != n_regions)
        throw std::logic_error("zero-cost EH: begin/end/landing-pad 标记数量不匹配");
    TagLower::EhPlanRec rec;
    rec.func_label = fname;
    for (size_t k = 0; k < n_regions; ++k)
        rec.ranges.push_back({tag_.eh_begin_labels[k], tag_.eh_end_labels[k], tag_.eh_lp_labels[k]});
    rec.lsda_index = static_cast<int>(tag_.eh_plans.size());
    tag_.eh_plans.push_back(std::move(rec));
    eh_plan_active_ = true;
}

void AsmGen::emit_eh_directives_for_current() {
    if (!eh_plan_active_ || tag_.eh_plans.empty()) return;
    const TagLower::EhPlanRec& rec = tag_.eh_plans.back();
    EhFuncPlan plan;
    plan.lsda_index = rec.lsda_index;
    plan.func_label = rec.func_label.c_str();
    for (const auto& rg : rec.ranges) {
        EhRange r;
        r.begin_label = rg.begin.c_str();
        r.end_label = rg.end.c_str();
        r.lp_label = rg.lp.c_str();
        r.is_handler = true;
        plan.ranges.push_back(r);
    }
    std::string text;
    emit_eh_func_directives(text, plan);
    emit_raw(text);
}

void AsmGen::emit_eh_tables() {
    if (tag_.eh_plans.empty()) return;
    std::string text;
    for (const auto& rec : tag_.eh_plans) {
        EhFuncPlan plan;
        plan.lsda_index = rec.lsda_index;
        plan.func_label = rec.func_label.c_str();
        for (const auto& rg : rec.ranges) {
            EhRange r;
            r.begin_label = rg.begin.c_str();
            r.end_label = rg.end.c_str();
            r.lp_label = rg.lp.c_str();
            r.is_handler = true;
            plan.ranges.push_back(r);
        }
        emit_eh_lsda(text, plan);
    }
    emit_eh_dwref(text);
    emit_raw(text);
}

std::string generate_asm(const IRModule& m, bool disable_unknown_index,
                         bool zero_cost_exc, bool index_readonly,
                         bool ra_opt,
                         bool inline_opt, bool type_tags, bool vect, bool needs_dynamic_big,
                         bool vect_fastfloat) {
    AsmGen g;
    // Erased big keys (and UNK shadow tags) rely on the live RC index, even
    // if the module that creates the BigInt has no local dynamic operations.
    g.set_disable_unknown_index(disable_unknown_index && !needs_dynamic_big);
    g.set_index_readonly(index_readonly && !needs_dynamic_big);
    g.set_zero_cost_exc(zero_cost_exc);
    g.set_ra_opt(ra_opt);
    g.set_inline_opt(inline_opt);
    g.set_type_tags(type_tags);
    g.set_vect(vect);
    g.set_vect_fastfloat(vect_fastfloat);
    return g.gen(m);
}

}  // namespace pyadamas
