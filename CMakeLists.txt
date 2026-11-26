cmake_minimum_required(VERSION 3.28)
project(pyadamas LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ccache：透明加速重复编译（clean 重建/切分支/CI/worktree 初始化实测 13×）。
# 自动探测，-DPYADAMAS_USE_CCACHE=OFF 可关。不改编译 flags，行为零变化；
# 真实源码改动的增量不受影响（改动 TU 必然 miss）。详见 docs/compile-speed.md。
option(PYADAMAS_USE_CCACHE "用 ccache 加速重复编译（自动探测，可关）" ON)
if(PYADAMAS_USE_CCACHE)
  find_program(PYADAMAS_CCACHE_EXECUTABLE ccache)
  if(PYADAMAS_CCACHE_EXECUTABLE)
    set(CMAKE_C_COMPILER_LAUNCHER "${PYADAMAS_CCACHE_EXECUTABLE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${PYADAMAS_CCACHE_EXECUTABLE}")
  endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(PYADAMAS_SOURCES
  src/main.cpp
  src/backend/tag.cpp
  src/backend/impl.cpp
  src/common/target_abi.cpp
  src/driver/util.cpp
  src/driver/link.cpp
  src/driver/build.cpp
  src/driver/build_cache.cpp
  src/driver/program_opts.cpp
  src/driver/progcache.cpp
  src/driver/modules.cpp
  src/driver/check.cpp
  src/driver/sha256.cpp
  src/driver/manifest.cpp
  src/driver/lock.cpp
  src/typecheck/builtin_sigs.cpp
  src/syntax/parser.cpp
  src/syntax/stmt.cpp
  src/syntax/func.cpp
  src/syntax/expr.cpp
  src/syntax/match.cpp
  src/typecheck/typecheck.cpp
  src/typecheck/annotation.cpp
  src/typecheck/stmt.cpp
  src/typecheck/cstruct.cpp
  src/typecheck/narrow.cpp
  src/typecheck/stmt_forms.cpp
  src/typecheck/expr.cpp
  src/typecheck/operators.cpp
  src/typecheck/expr_attr.cpp
  src/typecheck/call.cpp
  src/typecheck/mi_guard.cpp
  src/typecheck/infer.cpp
  src/typecheck/call_forms.cpp
  src/typecheck/call_modules.cpp
  src/typecheck/call_method.cpp
  src/typecheck/builtin.cpp
  src/typecheck/func.cpp
  src/typecheck/classdef.cpp
  src/typecheck/class_alias.cpp
  src/typecheck/dataclass.cpp
  src/typecheck/enum.cpp
  src/lower/lower.cpp
  src/lower/class.cpp
  src/lower/mi_thunk.cpp
  src/lower/impl_util.cpp
  src/lower/impl_slots.cpp
  src/lower/borrow.cpp
  src/lower/stmt.cpp
  src/lower/stmt_for.cpp
  src/lower/for_pairs.cpp
  src/lower/idiom.cpp
  src/lower/stmt_with.cpp
  src/lower/stmt_async.cpp
  src/lower/stmt_assign.cpp
  src/lower/exc.cpp
  src/lower/import.cpp
  src/lower/import_native.cpp
  src/lower/import_stdlib.cpp
  src/lower/import_misc.cpp
  src/lower/vthread.cpp
  src/lower/cpy.cpp
  src/lower/match.cpp
  src/lower/expr.cpp
  src/lower/expr_assign.cpp
  src/lower/expr_ops.cpp
  src/lower/expr_container.cpp
  src/lower/expr_dunder.cpp
  src/lower/comp.cpp
  src/lower/call.cpp
  src/lower/call_asyncio.cpp
  src/lower/c_lazy.cpp
  src/lower/builtin.cpp
  src/lower/builtin_reflect.cpp
  src/lower/builtin_seq.cpp
  src/lower/builtin_container.cpp
  src/lower/builtin_text.cpp
  src/lower/builtin_misc.cpp
  src/lower/aggregate.cpp
  src/lower/method.cpp
  src/lower/method_native.cpp
  src/lower/method_containers.cpp
  src/lower/method_str.cpp
  src/lower/method_misc.cpp
  src/lower/print.cpp
  src/lower/gen.cpp
  src/lower/gen_async.cpp
  src/lower/gen_spill.cpp
  src/opt/opt.cpp
  src/opt/forward.cpp
  src/opt/cse.cpp
  src/ir/verify.cpp
  src/opt/rc.cpp
    src/opt/dict_bump.cpp
    src/opt/dict_fuse.cpp
    src/opt/global_promote.cpp
    src/backend/live_interval.cpp
    src/backend/live_interval_build.cpp
  src/opt/fold.cpp
  src/opt/ifconv.cpp
  src/opt/inline.cpp
  src/opt/loop.cpp
src/opt/intloop.cpp
    src/opt/intloop_bce.cpp
  src/opt/devirt.cpp
  src/opt/stats.cpp
  src/backend/codegen.cpp
  src/backend/regalloc.cpp
  src/backend/regalloc_scan.cpp
  src/backend/regalloc_temp.cpp
  src/backend/ra_opt.cpp
  src/backend/block.cpp
  src/backend/fuse.cpp
  src/backend/comment.cpp
  src/backend/ops.cpp
  src/backend/store.cpp
  src/backend/div_const.cpp
  src/backend/int_fixed.cpp
  src/backend/big.cpp
  src/backend/call.cpp
  src/backend/call_abi.cpp
  src/backend/cffi.cpp
  src/backend/index.cpp
  src/backend/vcall_inline.cpp
  src/backend/vect.cpp
  src/backend/vect_affine.cpp
  src/backend/vect_avx2.cpp
  src/backend/vect_gates.cpp
  src/backend/vect_jvec.cpp
  src/opt/vect_planner.cpp
  src/opt/vect_planner_loop.cpp
  src/opt/vect_planner_alias.cpp
  src/backend/masked.cpp
  src/opt/vect_planner_stride.cpp
  src/opt/vect_planner_intfast.cpp
  src/opt/vect_rules_test.cpp
  src/backend/call_rt.cpp
  src/backend/call_rc.cpp
  src/backend/inline_containers.cpp
  src/backend/mach_emit.cpp
  src/ir/ir.cpp
  src/syntax/desugar.cpp
  src/syntax/desugar_scan.cpp
  src/ir/module.cpp
  src/ir/module_map.cpp
  src/ir/deps_json.cpp
  src/cimport/cimport.cpp
  src/cimport/cdecl.cpp
  src/cimport/cdecl_parse.cpp
  src/cimport/cdecl_types.cpp
  src/cimport/cdecl_eval.cpp
  src/backend/eh_emit.cpp
)

# 编译器在运行时需要 rt.o 的绝对路径；运行时按职责拆成多个 .c，
# 分别编译成可重定位对象后再用 ld -r 合成一个 rt.o。
set(PYADAMAS_RT_OBJ "${CMAKE_BINARY_DIR}/rt.o")
set(PYADAMAS_MODCACHE_DIR "${CMAKE_BINARY_DIR}/modcache")
option(PYADAMAS_WERROR "把编译告警当作错误（默认开）" ON)

# 运行时用：生成代码里存在调用点 %rsp 未按 16 字节对齐
# 的情况（协程/生成器路径）。GCC 15 在运行时函数里用 movaps 溢出 SSE，
# 调用方不对齐就直接 #GP 崩溃（Debian 13 + gcc 15 实测 24_async SIGSEGV；
# gcc 14 恰好没用 movaps，所以一直没暴露）。这是止血措施，根因仍需修
# codegen 的调用点对齐。
if(PYADAMAS_WERROR)
  set(PYADAMAS_RT_WERROR -Werror)
else()
  set(PYADAMAS_RT_WERROR "")
endif()

set(PYADAMAS_RT_PARTS core exc eh gen async task fd agen str list dict slice class math complex buffer format file random os subprocess time stdlib json re functools datetime deque hash debug personality diag cpy cffi vthread any lang big del templ)
# add_custom_command 不会自动继承 CMAKE_<LANG>_COMPILER_LAUNCHER；
# 手动把 ccache 之类的 launcher 接到运行时编译上（未设置时展开为空，行为不变）。
set(PYADAMAS_RT_LAUNCHER "")
if(CMAKE_C_COMPILER_LAUNCHER)
  set(PYADAMAS_RT_LAUNCHER ${CMAKE_C_COMPILER_LAUNCHER})
endif()
set(PYADAMAS_RT_OBJS "")
foreach(part ${PYADAMAS_RT_PARTS})
  set(rt_obj "${CMAKE_BINARY_DIR}/rt_${part}.o")
  # 运行时不再依赖帧指针：traceback 走 .eh_frame（libgcc 的
  # _Unwind_Backtrace），由编译器自己决定要不要 rbp。
  set(rt_fp "")
  # -ffunction-sections/-fdata-sections：每函数/每数据独立段（.text.foo /
  # .data.foo），链接期 --gc-sections 才能按函数剥掉用户程序未用到的运行时
  # （slim 轮；ld -r 合并保留段名粒度，已验证）。不带 --gc-sections 时链接器
  # 照旧把这些段并进 .text/.data，功能零变化。
  add_custom_command(
    OUTPUT "${rt_obj}"
    COMMAND ${PYADAMAS_RT_LAUNCHER} ${CMAKE_C_COMPILER} -O2 -Wall -Wextra ${PYADAMAS_RT_WERROR} ${rt_fp}
            -ffunction-sections -fdata-sections
            -I "${CMAKE_SOURCE_DIR}/runtime"
            -c "${CMAKE_SOURCE_DIR}/runtime/rt_${part}.c" -o "${rt_obj}"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/rt_${part}.c"
            "${CMAKE_SOURCE_DIR}/runtime/rt_internal.h"
    COMMENT "Building pyadamas runtime rt_${part}.o"
    VERBATIM
  )
  list(APPEND PYADAMAS_RT_OBJS "${rt_obj}")
endforeach()
add_custom_command(
  OUTPUT "${PYADAMAS_RT_OBJ}"
  COMMAND ${CMAKE_LINKER} -r -o "${PYADAMAS_RT_OBJ}" ${PYADAMAS_RT_OBJS}
  DEPENDS ${PYADAMAS_RT_OBJS}
  COMMENT "Combining pyadamas runtime rt.o"
  VERBATIM
)
add_custom_target(pyadamas_rt ALL DEPENDS "${PYADAMAS_RT_OBJ}")

# 插桩版运行时：给 `--asan` 用。
#
# 默认的 rt.o 不带任何 sanitizer 插桩，于是**运行时内部**（RC/GC/列表/
# 字典的指针操作）的内存错误 ASan 完全看不到 —— 只有生成代码那一侧被检查。
# 0.216 那个 bug 能抓到纯属运气：出错点在生成代码调 pa_rc_temp 的地方。
#
# 这里把同样的源文件用 -fsanitize=address 重编一遍，合成 rt_asan.o。
# 编译器在 --asan 模式下链接它而不是 rt.o。
set(PYADAMAS_RT_ASAN_OBJ "${CMAKE_BINARY_DIR}/rt_asan.o")
set(PYADAMAS_RT_ASAN_OBJS "")
foreach(part ${PYADAMAS_RT_PARTS})
  set(rt_obj "${CMAKE_BINARY_DIR}/rt_asan_${part}.o")
  add_custom_command(
    OUTPUT "${rt_obj}"
    # -Wno-clobbered：ASan + -O1 下 gcc 会对 setjmp 附近的局部变量报
    # 「might be clobbered by longjmp」。运行时的生成器/协程确实在 setjmp
    # 保护里用局部变量，但那是**有意**的（值都放在 jmp_buf 之外的帧槽），
    # 而且普通 rt.o 用 -O2 不报。这里只对插桩版关掉这个噪音。
    COMMAND ${PYADAMAS_RT_LAUNCHER} ${CMAKE_C_COMPILER} -O1 -g -fsanitize=address -fno-omit-frame-pointer -mstackrealign
            -ffunction-sections -fdata-sections
            -Wall -Wextra -Wno-clobbered ${PYADAMAS_RT_WERROR}
            -I "${CMAKE_SOURCE_DIR}/runtime"
            -c "${CMAKE_SOURCE_DIR}/runtime/rt_${part}.c" -o "${rt_obj}"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/rt_${part}.c"
            "${CMAKE_SOURCE_DIR}/runtime/rt_internal.h"
    COMMENT "Building pyadamas runtime rt_asan_${part}.o"
    VERBATIM
  )
  list(APPEND PYADAMAS_RT_ASAN_OBJS "${rt_obj}")
endforeach()
add_custom_command(
  OUTPUT "${PYADAMAS_RT_ASAN_OBJ}"
  COMMAND ${CMAKE_LINKER} -r -o "${PYADAMAS_RT_ASAN_OBJ}" ${PYADAMAS_RT_ASAN_OBJS}
  DEPENDS ${PYADAMAS_RT_ASAN_OBJS}
  COMMENT "Combining pyadamas runtime rt_asan.o"
  VERBATIM
)
add_custom_target(pyadamas_rt_asan ALL DEPENDS "${PYADAMAS_RT_ASAN_OBJ}")

# 分配器调试版运行时：canary / guard page（PYADAMAS_CANARY / PYADAMAS_GUARD）。
#
# 和 rt.o 用同一批源文件，多一个 -DPYADAMAS_ALLOC_DEBUG=1：
#   * 所有 rc_alloc* / 释放路径换成尾部 canary 或「尾对齐 + PROT_NONE
#     guard page」的分配器；
#   * 默认 rt.o 完全不含这些代码（钩子宏展开成裸 malloc/free），
#     所以两个开关关掉时生成程序的指令数与改动前逐项相同。
# 编译器只在环境变量非空时链接它而不是 rt.o（见 driver/build.cpp）；
# 典型用法：PYADAMAS_CANARY=1 bash tests/run_tests.sh ...
set(PYADAMAS_RT_DEBUG_OBJ "${CMAKE_BINARY_DIR}/rt_allocdbg.o")
set(PYADAMAS_RT_DEBUG_OBJS "")
foreach(part ${PYADAMAS_RT_PARTS})
  set(rt_obj "${CMAKE_BINARY_DIR}/rt_debug_${part}.o")
  add_custom_command(
    OUTPUT "${rt_obj}"
    COMMAND ${PYADAMAS_RT_LAUNCHER} ${CMAKE_C_COMPILER} -O2 -g -DPYADAMAS_ALLOC_DEBUG=1
            -ffunction-sections -fdata-sections
            -Wall -Wextra ${PYADAMAS_RT_WERROR}
            -I "${CMAKE_SOURCE_DIR}/runtime"
            -c "${CMAKE_SOURCE_DIR}/runtime/rt_${part}.c" -o "${rt_obj}"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/rt_${part}.c"
            "${CMAKE_SOURCE_DIR}/runtime/rt_internal.h"
    COMMENT "Building pyadamas runtime rt_debug_${part}.o"
    VERBATIM
  )
  list(APPEND PYADAMAS_RT_DEBUG_OBJS "${rt_obj}")
endforeach()
add_custom_command(
  OUTPUT "${PYADAMAS_RT_DEBUG_OBJ}"
  COMMAND ${CMAKE_LINKER} -r -o "${PYADAMAS_RT_DEBUG_OBJ}" ${PYADAMAS_RT_DEBUG_OBJS}
  DEPENDS ${PYADAMAS_RT_DEBUG_OBJS}
  COMMENT "Combining pyadamas runtime rt_allocdbg.o"
  VERBATIM
)
add_custom_target(pyadamas_rt_debug ALL DEPENDS "${PYADAMAS_RT_DEBUG_OBJ}")



option(PYADAMAS_PCH "预编译公共标准库头（每个配置独立 PCH，flags 自动匹配）" OFF)
option(PYADAMAS_MODULE_SCAN "C++23 模块扫描（项目未用 C++ modules；默认关：省 89 遍 -E 扫描、去掉 -fmodules-ts 注入、兼容 ccache）" OFF)

add_executable(pyadamas ${PYADAMAS_SOURCES})
if(PYADAMAS_PCH)
  # 只预编译标准库头（src/common/pch_std.h）。CMake 按 target 的编译 flags 为每个
  # 配置生成独立 PCH（dev/release 的 -O 各自匹配），不会重蹈跨 -O 复用
  # 被 GCC 静默拒绝的覆辙。全量收益≈0、增量 -14~-18%，见 docs/compile-speed.md。
  target_precompile_headers(pyadamas PRIVATE src/common/pch_std.h)
endif()
if(NOT PYADAMAS_MODULE_SCAN)
  # 源码里没有任何 C++ module 声明/import。CMake 4 对 C++23 默认开模块扫描：
  # 每个 TU 多一遍 -E 扫描（89 遍），编译命令注入 -fmodules-ts/-fmodule-mapper，
  # 且 ccache 见 -fmodules-ts 一律拒缓存。显式关闭对行为零影响。
  set_target_properties(pyadamas PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()
# 把插桩运行时的路径编进编译器，`--asan` 时链接它而不是 rt.o。
target_compile_definitions(pyadamas PRIVATE
  PYADAMAS_RT_ASAN_OBJ="${PYADAMAS_RT_ASAN_OBJ}"
  PYADAMAS_RT_DEBUG_OBJ="${PYADAMAS_RT_DEBUG_OBJ}")
target_include_directories(pyadamas PRIVATE src)
target_compile_options(pyadamas PRIVATE -Wall -Wextra)

# 改 CHANGELOG 必须触发重新 configure：版本字符串进 deps --json 与缓存键，
# 而 file(STRINGS) 只在 configure 阶段求值；不登记依赖的话 ninja 会判定
# 「无工作」，二进制一直报旧版本（0.476 之前实测报 0.469）。
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md")

# 编译器版本：从 CHANGELOG 首条 "## [x.y]" 推导，避免手抄版本号漂移。
# deps --json 的 compiler.version 直接用它（消费方据此判断兼容性）。
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md" _pa_ver_line
     REGEX "^## \\[[0-9]+\\.[0-9]+" LIMIT_COUNT 1)
if(_pa_ver_line)
  string(REGEX MATCH "[0-9]+\\.[0-9]+" PYADAMAS_VERSION "${_pa_ver_line}")
else()
  set(PYADAMAS_VERSION "dev")
endif()
message(STATUS "pyadamas version: ${PYADAMAS_VERSION}")

target_compile_definitions(pyadamas PRIVATE
  PYADAMAS_RT_OBJ="${PYADAMAS_RT_OBJ}"
  PYADAMAS_MODCACHE_DIR="${PYADAMAS_MODCACHE_DIR}"
  PYADAMAS_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib"
  PYADAMAS_VERSION="${PYADAMAS_VERSION}"
)
add_dependencies(pyadamas pyadamas_rt pyadamas_rt_debug)

# ── 直接调用 ld 链接（吞吐）────────────────────────────────────────
#
# 一次 build 的链接是 36 ms：ld.bfd 25 ms + gcc 驱动（specs 解析/库搜索/
# 两次 exec）11 ms。驱动那部分对「每次编译都链一次短命程序」是纯开销，
# 这里在配置期用 -print-file-name / -print-search-dirs 复刻出 gcc 实际
# 传给 ld 的命令行（crt 文件、-L 目录、-pie -z now -z relro --build-id），
# 运行期直接 exec ld。探测不到就留空前缀，驱动自动回退 cc（行为不变）。
option(PYADAMAS_DIRECT_LD "直接调用 ld 链接生成程序（省 gcc 驱动开销；探测失败自动回退 cc）" ON)
if(PYADAMAS_DIRECT_LD)
  find_program(PYADAMAS_LD_EXECUTABLE NAMES ld ld.bfd)
  if(PYADAMAS_LD_EXECUTABLE)
    set(_pa_crt_ok TRUE)
    foreach(_pa_crt Scrt1.o crti.o crtbeginS.o crtendS.o crtn.o)
      execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=${_pa_crt}
                      OUTPUT_VARIABLE _pa_p OUTPUT_STRIP_TRAILING_WHITESPACE)
      if(_pa_p STREQUAL _pa_crt OR NOT EXISTS "${_pa_p}")
        set(_pa_crt_ok FALSE)
      endif()
      set(_pa_${_pa_crt} "${_pa_p}")
    endforeach()
    # 汇编器路径（直调 as 省一次 gcc 驱动；找不到就用 cc -c）。
    execute_process(COMMAND ${CMAKE_C_COMPILER} -print-prog-name=as
                    OUTPUT_VARIABLE _pa_as OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_pa_as AND EXISTS "${_pa_as}")
      get_filename_component(PYADAMAS_AS_EXECUTABLE "${_pa_as}" REALPATH)
    endif()
    # 装载器路径必须与 gcc 写进 .interp 的逐字节一致（否则二进制不同）：
    # gcc 的 specs 固定用 /lib64/ld-linux-x86-64.so.2。-print-file-name 返回的
    # 是未经归一化的长路径（/usr/lib/gcc/.../../../../x86_64-linux-gnu/...），
    # 直接用它会让 .interp 变长、后续所有段偏移都变。
    if(EXISTS "/lib64/ld-linux-x86-64.so.2")
      set(_pa_loader "/lib64/ld-linux-x86-64.so.2")
    else()
      execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=ld-linux-x86-64.so.2
                      OUTPUT_VARIABLE _pa_loader OUTPUT_STRIP_TRAILING_WHITESPACE)
      get_filename_component(_pa_loader "${_pa_loader}" REALPATH)
    endif()
    execute_process(COMMAND ${CMAKE_C_COMPILER} -print-search-dirs
                    OUTPUT_VARIABLE _pa_dirs)
    set(_pa_libdirs "")
    if(_pa_dirs MATCHES "libraries: =([^\n]+)")
      string(REPLACE ":" ";" _pa_list "${CMAKE_MATCH_1}")
      foreach(_pa_d IN LISTS _pa_list)
        if(EXISTS "${_pa_d}")
          string(APPEND _pa_libdirs " -L${_pa_d}")
        endif()
      endforeach()
    endif()
    if(_pa_crt_ok AND EXISTS "${_pa_loader}")
      set(PYADAMAS_LD_FLAGS
          "--eh-frame-hdr --build-id -m elf_x86_64 --hash-style=gnu --as-needed -dynamic-linker ${_pa_loader} -pie -z now -z relro${_pa_libdirs} ${_pa_Scrt1.o} ${_pa_crti.o} ${_pa_crtbeginS.o}")
      set(PYADAMAS_LD_TAIL
          "-lgcc --push-state --as-needed -lgcc_s --pop-state -lc -lgcc --push-state --as-needed -lgcc_s --pop-state ${_pa_crtendS.o} ${_pa_crtn.o}")
      message(STATUS "pyadamas: 直接 ld 链接已启用（${PYADAMAS_LD_EXECUTABLE}）")
    else()
      message(STATUS "pyadamas: 直接 ld 链接探测失败，继续用 cc 链接")
    endif()
  endif()
endif()
# 探测结果编进编译器（driver/link.cpp 用；探测失败时为空串 → 回退 cc）。
target_compile_definitions(pyadamas PRIVATE
  PYADAMAS_AS_EXECUTABLE="${PYADAMAS_AS_EXECUTABLE}"
  PYADAMAS_LD_EXECUTABLE="${PYADAMAS_LD_EXECUTABLE}"
  PYADAMAS_LD_FLAGS="${PYADAMAS_LD_FLAGS}"
  PYADAMAS_LD_TAIL="${PYADAMAS_LD_TAIL}"
)

# 静态链接 libstdc++（吞吐）：编译器每次被调用都要经动态装载器解析
# libstdc++.so.6 的 ~800 个符号（实测占进程启动 ~1.2 ms，占一次
# "pyadamas check" 的 ~30%）。静态链接后启动指令数 5.0M→3.2M、cycles
# -27%，tests/run 227 例逐文件 check 473.9→365.9 ms（-22.8%）。
# 不改任何代码生成/运行时行为（生成的程序仍动态链接），-DPYADAMAS_STATIC_LIBSTDCXX=OFF 可关。
option(PYADAMAS_STATIC_LIBSTDCXX "静态链接 libstdc++，省编译器进程启动的符号解析" ON)
if(PYADAMAS_STATIC_LIBSTDCXX AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(pyadamas PRIVATE -static-libstdc++)
endif()


if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(pyadamas PRIVATE -Wshadow -Wnon-virtual-dtor
                                      -Wold-style-cast -Wcast-qual)
  if(PYADAMAS_WERROR)
    target_compile_options(pyadamas PRIVATE -Werror)
  endif()
endif()

enable_testing()
find_program(BASH_EXECUTABLE bash REQUIRED)
add_test(
  NAME pyadamas_suite
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/run_tests.sh"
          "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:pyadamas>"
)
set_tests_properties(pyadamas_suite PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  TIMEOUT 900
)

add_test(
  NAME pyadamas_opt_levels
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/opt_levels.sh"
          "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:pyadamas>"
)
set_tests_properties(pyadamas_opt_levels PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  TIMEOUT 600
)

add_test(
  NAME pyadamas_cache_race
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/cache_race.sh"
          "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:pyadamas>"
)
set_tests_properties(pyadamas_cache_race PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  RUN_SERIAL TRUE
  TIMEOUT 300
)

# SHA-256 的已知答案测试：锁的树哈希是信任根，算法错了「两次输出相同」查不出来。
add_executable(pyadamas_sha256_test tests/pkg/sha256_test.cpp src/driver/sha256.cpp)
target_include_directories(pyadamas_sha256_test PRIVATE src)
add_test(NAME pyadamas_pkg_sha256 COMMAND pyadamas_sha256_test)

# 包管理 P0（清单/锁/resolve/--locked/锁派生模块图）。失败时先看 tests/pkg/run.sh 的
# 检查名与命令回显；脚本自带临时工作区，不依赖仓库里的文件。
add_test(
  NAME pyadamas_pkg
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/pkg/run.sh"
          "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:pyadamas>"
)
set_tests_properties(pyadamas_pkg PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  TIMEOUT 300
)

# 整程序产物缓存（0.478）：键必须覆盖全部链接输入——同输入 hit、任一项变化 miss。
# 脚本自带 PYADAMAS_CACHE_DIR 隔离，不碰仓库/用户的缓存。
add_test(
  NAME pyadamas_progcache
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/progcache.sh" "$<TARGET_FILE:pyadamas>"
)
set_tests_properties(pyadamas_progcache PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  TIMEOUT 300
)

# check --imports 的口径（0.486）：模块属性检查必须保持 opt-in（默认打开会因 stdlib
# 签名不全误杀真实代码：实测 20 个 CPython 标准库文件 12/20 → 5/20），同时
# --imports 下「模块没有这个属性」与 os.path 子模块链两条行为都要钉住。
add_test(
  NAME pyadamas_check_imports
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/check_imports.sh"
          "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:pyadamas>"
)
add_test(
  NAME pyadamas_gc_conservative
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/gc_conservative.sh"
          "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:pyadamas>"
)
set_tests_properties(pyadamas_gc_conservative PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  TIMEOUT 300
)

set_tests_properties(pyadamas_check_imports PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  TIMEOUT 300
)

# traceback 行号（0.494）：未捕获异常的回溯必须给真实行号（以前是 line 0）。
# 回溯打在 stderr，tests/run 的 expect 只比 stdout，所以单独成一个脚本用例。
add_test(
  NAME pyadamas_traceback_lines
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/traceback_lines.sh"
          "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:pyadamas>"
)
set_tests_properties(pyadamas_traceback_lines PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  TIMEOUT 300
)

add_custom_target(clean-modcache
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${PYADAMAS_MODCACHE_DIR}"
  COMMENT "Removing pyadamas module cache"
)
