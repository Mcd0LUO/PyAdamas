cmake_minimum_required(VERSION 3.28)
project(pyadamas LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(PYADAMAS_SOURCES
  src/main.cpp
  src/target_abi.cpp
  src/driver_util.cpp
  src/driver_build.cpp
  src/builtin_sigs.cpp
  src/parser.cpp
  src/parser_stmt.cpp
  src/parser_func.cpp
  src/parser_expr.cpp
  src/parser_match.cpp
  src/typecheck.cpp
  src/typecheck_stmt.cpp
  src/typecheck_stmt_forms.cpp
  src/typecheck_expr.cpp
  src/typecheck_call.cpp
  src/typecheck_call_forms.cpp
  src/typecheck_call_modules.cpp
  src/typecheck_call_method.cpp
  src/typecheck_builtin.cpp
  src/typecheck_func.cpp
  src/lower.cpp
  src/lower_class.cpp
  src/lower_impl_util.cpp
  src/lower_impl_slots.cpp
  src/lower_stmt.cpp
  src/lower_stmt_with.cpp
  src/lower_stmt_async.cpp
  src/lower_exc.cpp
  src/lower_import.cpp
  src/lower_import_native.cpp
  src/lower_import_stdlib.cpp
  src/lower_import_misc.cpp
  src/lower_match.cpp
  src/lower_expr.cpp
  src/lower_expr_assign.cpp
  src/lower_expr_ops.cpp
  src/lower_expr_container.cpp
  src/lower_expr_dunder.cpp
  src/lower_comp.cpp
  src/lower_call.cpp
  src/lower_builtin.cpp
  src/lower_builtin_reflect.cpp
  src/lower_builtin_seq.cpp
  src/lower_builtin_container.cpp
  src/lower_builtin_text.cpp
  src/lower_builtin_misc.cpp
  src/lower_aggregate.cpp
  src/lower_method.cpp
  src/lower_method_native.cpp
  src/lower_method_containers.cpp
  src/lower_method_str.cpp
  src/lower_method_misc.cpp
  src/lower_print.cpp
  src/lower_gen.cpp
  src/lower_gen_async.cpp
  src/lower_gen_spill.cpp
  src/opt.cpp
  src/opt_forward.cpp
  src/opt_cse.cpp
  src/opt_rc.cpp
    src/opt_dict_bump.cpp
    src/opt_global_promote.cpp
    src/spill_placement.cpp
    src/live_interval.cpp
    src/live_interval_build.cpp
  src/opt_fold.cpp
  src/opt_inline.cpp
  src/opt_loop.cpp
  src/codegen.cpp
  src/codegen_regalloc.cpp
  src/codegen_block.cpp
  src/codegen_ops.cpp
  src/codegen_call.cpp
  src/codegen_call_rt.cpp
  src/mach_emit.cpp
  src/ir.cpp
  src/desugar.cpp
  src/module.cpp
  src/cimport.cpp
  src/cdecl.cpp
  src/cdecl_parse.cpp
  src/eh_emit.cpp
)

# 编译器在运行时需要 rt.o 的绝对路径；运行时按职责拆成多个 .c，
# 分别编译成可重定位对象后再用 ld -r 合成一个 rt.o。
set(PYADAMAS_RT_OBJ "${CMAKE_BINARY_DIR}/rt.o")
set(PYADAMAS_MODCACHE_DIR "${CMAKE_BINARY_DIR}/modcache")
option(PYADAMAS_WERROR "把编译告警当作错误（默认开）" ON)

if(PYADAMAS_WERROR)
  set(PYADAMAS_RT_WERROR -Werror)
else()
  set(PYADAMAS_RT_WERROR "")
endif()

set(PYADAMAS_RT_PARTS core exc eh gen async task fd agen str list dict slice class math complex format file random os subprocess time stdlib json re functools datetime deque hash debug personality diag)
set(PYADAMAS_RT_OBJS "")
foreach(part ${PYADAMAS_RT_PARTS})
  set(rt_obj "${CMAKE_BINARY_DIR}/rt_${part}.o")
  # 运行时不再依赖帧指针：traceback 走 .eh_frame（libgcc 的
  # _Unwind_Backtrace），由编译器自己决定要不要 rbp。
  set(rt_fp "")
  add_custom_command(
    OUTPUT "${rt_obj}"
    COMMAND ${CMAKE_C_COMPILER} -O2 -Wall -Wextra ${PYADAMAS_RT_WERROR} ${rt_fp}
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
    COMMAND ${CMAKE_C_COMPILER} -O1 -g -fsanitize=address -fno-omit-frame-pointer
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


add_executable(pyadamas ${PYADAMAS_SOURCES})
# 把插桩运行时的路径编进编译器，`--asan` 时链接它而不是 rt.o。
target_compile_definitions(pyadamas PRIVATE
  PYADAMAS_RT_ASAN_OBJ="${PYADAMAS_RT_ASAN_OBJ}")
target_include_directories(pyadamas PRIVATE src)
target_compile_options(pyadamas PRIVATE -Wall -Wextra)
target_compile_definitions(pyadamas PRIVATE
  PYADAMAS_RT_OBJ="${PYADAMAS_RT_OBJ}"
  PYADAMAS_MODCACHE_DIR="${PYADAMAS_MODCACHE_DIR}"
  PYADAMAS_STDLIB_DIR="${CMAKE_SOURCE_DIR}/stdlib"
)
add_dependencies(pyadamas pyadamas_rt)


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

add_custom_target(clean-modcache
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${PYADAMAS_MODCACHE_DIR}"
  COMMENT "Removing pyadamas module cache"
)
