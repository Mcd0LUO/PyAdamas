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
  src/typecheck.cpp
  src/lower.cpp
  src/codegen.cpp
  src/ir.cpp
  src/desugar.cpp
  src/module.cpp
)

# 编译器在运行时需要 rt.o 的绝对路径；用自定义命令生成一个稳定路径，
# 而不是把 OBJECT 库的中间路径拼进宏里。
set(PY23_RT_OBJ "${CMAKE_BINARY_DIR}/rt.o")
set(PY23_MODCACHE_DIR "${CMAKE_BINARY_DIR}/modcache")
add_custom_command(
  OUTPUT "${PY23_RT_OBJ}"
  COMMAND ${CMAKE_C_COMPILER} -O2 -Wall -Wextra -c
          "${CMAKE_SOURCE_DIR}/runtime/rt.c" -o "${PY23_RT_OBJ}"
  DEPENDS "${CMAKE_SOURCE_DIR}/runtime/rt.c"
  COMMENT "Building py23 runtime rt.o"
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

add_custom_target(clean-modcache
  COMMAND ${CMAKE_COMMAND} -E remove_directory "${PY23_MODCACHE_DIR}"
  COMMENT "Removing py23 module cache"
)
