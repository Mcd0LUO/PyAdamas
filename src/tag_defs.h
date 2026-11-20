// tag_defs.h —— unknown 值平行 tag 的编码与静态类型映射（P3 + P2 扩展）
//
// 从 codegen_impl.h 拆出（文件规模硬限制 1200 行）。tag 是编译器内部
// 旁路：值仍是 8B i64，UNK(0) 一律回退探针（正确性不依赖传播完备）。
//
// P3（默认，docs/p3-tagged-union-design.md §3.1）：
//   UNK/INT/FRAW/FBOX/STR/HEAP，堆 ⇔ tag>=3。
// P2（PYADAMAS_TYPE_TAGS，docs/numeric-types-design.md §2）在**不改动 P3
// 既有取值**的前提下追加：
//   非堆标量：NONE=6 / BOOL=7（None/Bool/Int 不再碰撞）
//   堆子类  ：TUPLE=8 / SET=9 / BYTES=10 / CLASS=11 / BIG=12（预留）
// 堆判定因此变成「3..5 或 >=8」。新 tag 只在 type_tags_ 下发；门控关时
// tag 编号与判定（堆 ⇔ tag>=3）逐字旧样，asm 逐字节不变。
// 与 runtime/rt_any.c 的 PA_TAG_* 逐字对应。
#pragma once

#include <cstdint>

#include "ir_types.h"

namespace pyadamas {

enum PaTag : int64_t {
    kTagUnk = 0, kTagInt = 1, kTagFraw = 2,
    kTagFbox = 3, kTagStr = 4, kTagHeap = 5,
    kTagNone = 6, kTagBool = 7,
    kTagTuple = 8, kTagSet = 9, kTagBytes = 10, kTagClass = 11,
    kTagBig = 12,  // 预留：P3 big BigInt 落地时用（堆）
};

// 静态类型导出的 tag（typed vreg 作 unknown 实参/存储源时用）。
// Optional 的机器表示是「内层值或 0」，内层可能是指针也可能是标量，
// 保守 UNK。Complex/Struct 等堆对象按 HEAP。
// type_tags=true 时 None/Bool 各得专 tag；Tuple/Set/Bytes/类对象得到精确
// 堆子类 tag —— 这些子类在运行期头 kind 里分不开（tuple 与 list 同为
// LIST、set 与 dict 同为 DICT、bytes 与 str 同为 STR、类对象藏在 OBJ 里），
// 静态类型是唯一可靠来源。
inline int64_t static_tag_of(const TypePtr& t, bool type_tags) {
    if (!t) return kTagUnk;
    switch (t->kind) {
        case TyKind::Int: return kTagInt;
        case TyKind::Bool: return type_tags ? kTagBool : kTagInt;
        case TyKind::None: return type_tags ? kTagNone : kTagInt;
        case TyKind::Float: return kTagFraw;
        case TyKind::Str: return kTagStr;
        case TyKind::Unknown: case TyKind::Optional: return kTagUnk;
        case TyKind::Tuple: if (type_tags) return kTagTuple; break;
        case TyKind::Set: if (type_tags) return kTagSet; break;
        case TyKind::Bytes: if (type_tags) return kTagBytes; break;
        case TyKind::ClassObj: case TyKind::TypeObj:
            if (type_tags) return kTagClass;
            break;
        default: break;
    }
    return ir_is_heap(t) ? kTagHeap : kTagUnk;
}

// 运行期类型码（pa_any_type 的返回值 / pa_any_type_is 的谓词实参）。
// 与 runtime/rt_any.c 的 PA_TYPE_* 逐字对应（docs/numeric-types-design.md §2.2）。
// isinstance(x, int) 对 BOOL 也为真（CPython 里 bool 是 int 的子类），由
// pa_any_type_is 处理；object 匹配一切。
enum PaTypeCode : int64_t {
    kTypeNone = 0,
    kTypeBool = 1,
    kTypeInt = 2,
    kTypeFloat = 3,
    kTypeStr = 4,
    kTypeBytes = 5,
    kTypeList = 6,
    kTypeDict = 7,
    kTypeTuple = 8,
    kTypeSet = 9,
    kTypeObject = 10,
    kTypeClass = 11,
    kTypeOther = 12,
};

}  // namespace pyadamas
