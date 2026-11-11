cmake_minimum_required(VERSION 3.10)
project(echo_native CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(ECHO_CXX_FLAGS "-O3 -g -Wall -march=native")
separate_arguments(ECHO_CXX_FLAGS_LIST UNIX_COMMAND "${ECHO_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${ECHO_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Vendored base (RapidsAtHKUST/SubgraphMatching @ bcaa935) with the ECHO hook patch applied.
set(SM ${CMAKE_CURRENT_SOURCE_DIR}/vendor/SubgraphMatching)
file(GLOB SM_SRC ${SM}/graph/*.cpp ${SM}/matching/*.cpp ${SM}/utility/*.cpp
     ${SM}/utility/han/*.cpp ${SM}/utility/han/utils/*.cpp)
list(FILTER SM_SRC EXCLUDE REGEX ".*/StudyPerformance\\.cpp$")
add_library(sm_base STATIC ${SM_SRC})
target_include_directories(sm_base PUBLIC ${SM} ${CMAKE_CURRENT_SOURCE_DIR}/include)


add_library(echo_core STATIC src/echo_common.cpp src/echo_dpiso.cpp src/echo_envelope.cpp src/echo_mask.cpp)
target_include_directories(echo_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${SM})
target_link_libraries(echo_core PUBLIC sm_base)

set(ECHO_CORE_LIBS echo_core)
add_subdirectory(tools)

# Existing MASC transplant, shared IO/predicate contract.
add_library(masc_core STATIC src/masc_filter.cpp src/masc_search.cpp)
target_include_directories(masc_core PUBLIC include ${SM})
target_link_libraries(masc_core PUBLIC echo_core)
add_executable(masc_main tools/masc.cpp)
target_link_libraries(masc_main PRIVATE masc_core)
