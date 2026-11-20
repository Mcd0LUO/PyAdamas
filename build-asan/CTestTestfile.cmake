# CMake generated Testfile for 
# Source directory: /tmp/wt-vect1
# Build directory: /tmp/wt-vect1/build-asan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[pyadamas_suite]=] "/usr/bin/bash" "/tmp/wt-vect1/tests/run_tests.sh" "/tmp/wt-vect1/build-asan" "/tmp/wt-vect1/build-asan/pyadamas")
set_tests_properties([=[pyadamas_suite]=] PROPERTIES  TIMEOUT "900" WORKING_DIRECTORY "/tmp/wt-vect1" _BACKTRACE_TRIPLES "/tmp/wt-vect1/CMakeLists.txt;347;add_test;/tmp/wt-vect1/CMakeLists.txt;0;")
add_test([=[pyadamas_opt_levels]=] "/usr/bin/bash" "/tmp/wt-vect1/tests/opt_levels.sh" "/tmp/wt-vect1/build-asan" "/tmp/wt-vect1/build-asan/pyadamas")
set_tests_properties([=[pyadamas_opt_levels]=] PROPERTIES  TIMEOUT "600" WORKING_DIRECTORY "/tmp/wt-vect1" _BACKTRACE_TRIPLES "/tmp/wt-vect1/CMakeLists.txt;357;add_test;/tmp/wt-vect1/CMakeLists.txt;0;")
add_test([=[pyadamas_cache_race]=] "/usr/bin/bash" "/tmp/wt-vect1/tests/cache_race.sh" "/tmp/wt-vect1/build-asan" "/tmp/wt-vect1/build-asan/pyadamas")
set_tests_properties([=[pyadamas_cache_race]=] PROPERTIES  RUN_SERIAL "TRUE" TIMEOUT "300" WORKING_DIRECTORY "/tmp/wt-vect1" _BACKTRACE_TRIPLES "/tmp/wt-vect1/CMakeLists.txt;367;add_test;/tmp/wt-vect1/CMakeLists.txt;0;")
