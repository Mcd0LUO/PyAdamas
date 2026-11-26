// typecheck/stmt.cpp —— 语句类型检查：控制流、try/with、赋值与解包
#include "driver/build.h"
#include "typecheck/typecheck.h"

#include <filesystem>
#include <set>

#include "ir/module.h"


namespace pyadamas {

// C 结构体声明（build_struct / build_cstruct）：cimport 与 .cpya 的结构体建模

void TypeChecker::build_struct(StructDef& s) {
        if (structs_.count(s.name)) {
            err(s.line, s.col, std::format("struct '{}' is already defined", s.name));
            return;
        }
        auto info = std::make_unique<StructInfo>();
        info->name = s.name;
        // v1 的字段映射固定：int → int64_t(8/8)、float → double(8/8)、
        // bool → _Bool(1/1)。布局算法与 C 一致：按字段对齐向上取整放置，
        // 总大小对齐到最大成员对齐。
        int64_t off = 0;
        int64_t max_align = 1;
        for (auto& f : s.fields) {
            if (info->field(f.name)) {
                err(f.line, f.col,
                    std::format("duplicate field '{}' in struct '{}'", f.name, s.name));
                continue;
            }
            auto* nm = dynamic_cast<Name*>(f.type.get());
            TypePtr ft;
            if (nm && nm->id == "int") ft = t_int();
            else if (nm && nm->id == "float") ft = t_float();
            else if (nm && nm->id == "bool") ft = t_bool();
            else {
                err(f.line, f.col, "struct field type must be 'int', 'float' or 'bool'");
                continue;
            }
            const int64_t sz = ft->kind == TyKind::Bool ? 1 : 8;
            const int64_t al = sz;
            off = (off + al - 1) / al * al;
            StructFieldInfo sfi;
            sfi.name = f.name;
            sfi.ty = ft;
            sfi.offset = off;
            sfi.size = sz;
            sfi.align = al;
            info->fields.push_back(std::move(sfi));
            off += sz;
            if (al > max_align) max_align = al;
        }
        info->align = max_align;
        info->size = (off + max_align - 1) / max_align * max_align;
        StructInfo* raw = info.get();
        all_structs_.push_back(std::move(info));
        structs_[s.name] = raw;
        module_->vars[s.name] = Symbol{mk_struct(raw), s.line, true, false};
    }

StructInfo* TypeChecker::build_cstruct(const CStruct& cs) {
        auto info = std::make_unique<StructInfo>();
        info->name = cs.name;
        info->size = cs.size;
        info->align = cs.align;
        info->c_struct = true;
        info->c_union = cs.is_union;
        for (auto& f : cs.fields) {
            TypePtr ft = t_int();
            bool unsig = false;
            if (!f.ptr_struct.empty()) {
                // 元素/字段是 .cpya 里声明的 struct/union（按值或 ptr[...]）。
                StructInfo* est = struct_info(f.ptr_struct);
                if (!est) continue;  // 依赖序保证不会发生
                ft = f.kind == CKind::Ptr ? mk_optional(mk_struct_ptr(est)) : mk_struct(est);
                StructFieldInfo sfi;
                sfi.name = f.name;
                sfi.ty = ft;
                sfi.offset = f.offset;
                sfi.size = f.size;
                sfi.align = f.align;
                sfi.dims = f.dims;
                sfi.c_elem = CKind::Struct;
                info->fields.push_back(std::move(sfi));
                continue;
            }
            switch (f.kind) {
                case CKind::Int:
                case CKind::Int32: ft = t_int(); break;
                case CKind::UInt32: ft = t_int(); unsig = true; break;
                case CKind::Int8: ft = t_i8(); break;
                case CKind::Int16: ft = t_i16(); break;
                case CKind::UInt8: ft = t_u8(); unsig = true; break;
                case CKind::UInt16: ft = t_u16(); unsig = true; break;
                case CKind::UInt64: ft = t_u64(); unsig = true; break;
                case CKind::Float: ft = t_float(); break;
                case CKind::Float32: ft = t_float(); break;
                case CKind::Bool: ft = t_bool(); break;
                case CKind::Ptr: ft = mk_optional(t_int()); break;
                default: break;
            }
            StructFieldInfo sfi;
            sfi.name = f.name;
            sfi.ty = ft;
            sfi.offset = f.offset;
            sfi.size = f.size;
            sfi.align = f.align;
            sfi.c_unsigned = unsig;
            sfi.dims = f.dims;
            sfi.c_elem = f.dims.empty() ? CKind::None : f.kind;
            if (f.kind == CKind::Ptr && f.ptr_elem != CKind::None) sfi.c_elem = f.ptr_elem;
            info->fields.push_back(std::move(sfi));
        }
        StructInfo* raw = info.get();
        all_structs_.push_back(std::move(info));
        return raw;
    }


}  // namespace pyadamas
