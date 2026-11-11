cmake_minimum_required(VERSION 3.16)
project(echo_v3 CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(NOT DEFINED ECHO_MARCH)
  set(ECHO_MARCH native)
endif()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g -Wall -Wextra -march=${ECHO_MARCH}")

# the base (v2 vendor/SubgraphMatching @ bcaa935, compiled by ../build as libsm_base.a and
# libecho_core.a) is linked in: v3_base.cpp is the shared base's per-query reference
if(NOT DEFINED V3_BASE_BUILD)
  set(V3_BASE_BUILD ${CMAKE_CURRENT_SOURCE_DIR}/../build)
endif()
add_library(v3_core STATIC
  src/v3_json.cpp src/v3_plan.cpp src/v3_graph.cpp src/v3_core.cpp
  src/v3_align.cpp src/v3_target.cpp src/v3_exec.cpp src/v3_executor.cpp src/v3_search.cpp src/v3_vl.cpp src/v3_match.cpp src/v3_depth_chain.cpp
  src/v3_mcs.cpp src/v3_base.cpp src/v3_base_pure.cpp src/v3_masc_ldbc.cpp
  src/v3_point.cpp)
target_include_directories(v3_core PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../vendor/SubgraphMatching)
target_link_libraries(v3_core PUBLIC ${V3_BASE_BUILD}/libecho_core.a ${V3_BASE_BUILD}/libsm_base.a pthread)

add_executable(v3_run tools/v3_run.cpp)
target_link_libraries(v3_run PRIVATE v3_core)

add_executable(v3_batch tools/v3_batch.cpp)
target_link_libraries(v3_batch PRIVATE v3_core)

add_executable(v3_batch_main33 tools/v3_batch.cpp)
target_link_libraries(v3_batch_main33 PRIVATE v3_core)

add_executable(v3_sidecar tools/v3_sidecar.cpp src/v3_json.cpp)
target_include_directories(v3_sidecar PRIVATE include)

add_executable(v3_cache tools/v3_cache.cpp)
target_link_libraries(v3_cache PRIVATE v3_core)

add_executable(v3_dump tools/v3_dump.cpp)
target_link_libraries(v3_dump PRIVATE v3_core)

add_executable(v3_check tools/v3_check.cpp)
target_link_libraries(v3_check PRIVATE v3_core)

add_executable(v3_p3 tools/v3_p3.cpp)
target_link_libraries(v3_p3 PRIVATE v3_core)
