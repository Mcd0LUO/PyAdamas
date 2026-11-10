cmake_minimum_required(VERSION 3.28)
project(py23 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

set(PY23_SOURCES
  src/main.cpp
  src/parser.cpp
  src/parser_stmt.cpp
  src/parser_func.cpp
  src/parser_expr.cpp
  src/parser_match.cpp
  src/typecheck.cpp
  src/typecheck_stmt.cpp
  src/typecheck_expr.cpp
  src/typecheck_call.cpp
  src/typecheck_builtin.cpp
  src/typecheck_func.cpp
  src/lower.cpp
  src/lower_stmt.cpp
  src/lower_exc.cpp
  src/lower_import.cpp
  src/lower_match.cpp
  src/lower_expr.cpp
  src/lower_comp.cpp
  src/lower_call.cpp
  src/lower_builtin.cpp
  src/lower_method.cpp
  src/lower_print.cpp
  src/lower_gen.cpp
  src/opt.cpp
  src/opt_fold.cpp
  src/opt_inline.cpp
  src/opt_loop.cpp
  src/codegen.cpp
  src/codegen_regalloc.cpp
  src/codegen_block.cpp
  src/codegen_ops.cpp
  src/codegen_call.cpp
  src/ir.cpp
  src/desugar.cpp
  src/module.cpp
)

# 编译器在运行时需要 rt.o 的绝对路径；运行时按职责拆成多个 .c，
# 分别编译成可重定位对象后再用 ld -r 合成一个 rt.o。
set(PY23_RT_OBJ "${CMAKE_BINARY_DIR}/rt.o")
set(PY23_MODCACHE_DIR "${CMAKE_BINARY_DIR}/modcache")
set(PY23_RT_PARTS core exc gen async task fd agen obj)
set(PY23_RT_OBJS "")
foreach(part ${PY23_RT_PARTS})
  set(rt_obj "${CMAKE_BINARY_DIR}/rt_${part}.o")
  add_custom_command(
    OUTPUT "${rt_obj}"
    COMMAND ${CMAKE_C_COMPILER} -O2 -Wall -Wextra
            -I "${CMAKE_SOURCE_DIR}/runtime"
            -c "${CMAKE_SOURCE_DIR}/runtime/rt_${part}.c" -o "${rt_obj}"
    DEPENDS "${CMAKE_SOURCE_DIR}/runtime/rt_${part}.c"
            "${CMAKE_SOURCE_DIR}/runtime/rt_internal.h"
    COMMENT "Building py23 runtime rt_${part}.o"
    VERBATIM
  )
  list(APPEND PY23_RT_OBJS "${rt_obj}")
endforeach()
add_custom_command(
  OUTPUT "${PY23_RT_OBJ}"
  COMMAND ${CMAKE_LINKER} -r -o "${PY23_RT_OBJ}" ${PY23_RT_OBJS}
  DEPENDS ${PY23_RT_OBJS}
  COMMENT "Combining py23 runtime rt.o"
  VERBATIM
)
add_custom_target(py23_rt ALL DEPENDS "${PY23_RT_OBJ}")

add_executable(py23 ${PY23_SOURCES})
target_include_directories(py23 PRIVATE src)
target_compile_options(py23 PRIVATE -Wall -Wextra)
target_compile_definitions(py23 PRIVATE
  PY23_RT_OBJ="${PY23_RT_OBJ}"
  PY23_MODCACHE_DIR="${PY23_MODCACHE_DIR}"
)
add_dependencies(py23 py23_rt)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(py23 PRIVATE -Wshadow -Wnon-virtual-dtor
                                      -Wold-style-cast -Wcast-qual)
endif()

enable_testing()
find_program(BASH_EXECUTABLE bash REQUIRED)
add_test(
  NAME py23_suite
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/run_tests.sh"
          "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:py23>"
)
set_tests_properties(py23_suite PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  TIMEOUT 900
)

add_test(
  NAME py23_opt_levels
  COMMAND "${BASH_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/opt_levels.sh"
          "${CMAKE_BINARY_DIR}" "$<TARGET_FILE:py23>"
)
set_tests_properties(py23_opt_levels PROPERTIES
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  TIMEOUT 600
)

add_custom_target(clean-modcache
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${PY23_MODCACHE_DIR}"
  COMMENT "Removing py23 module cache"
)
