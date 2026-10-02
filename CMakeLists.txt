cmake_minimum_required(VERSION 3.20)
project(full_orderbook_lab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(OBL_NATIVE "Compile with -march=native" ON)
option(OBL_SANITIZE "Build with address/undefined sanitizers" OFF)
option(OBL_WITH_ABSEIL "Fetch abseil for absl::btree_map / absl::flat_hash_map variants" ON)
option(OBL_WITH_UNORDERED_DENSE "Fetch ankerl::unordered_dense index variant" ON)
option(OBL_WITH_MIMALLOC "Fetch and build libmimalloc.so (used via LD_PRELOAD)" ON)

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

if(OBL_WITH_UNORDERED_DENSE)
  FetchContent_Declare(unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG v5.2.0
    GIT_SHALLOW TRUE
    SYSTEM)
  FetchContent_MakeAvailable(unordered_dense)
endif()

if(OBL_WITH_ABSEIL)
  set(ABSL_PROPAGATE_CXX_STD ON)
  set(ABSL_BUILD_TESTING OFF)
  set(ABSL_ENABLE_INSTALL OFF)
  FetchContent_Declare(abseil
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG 20260526.0
    GIT_SHALLOW TRUE
    SYSTEM)
  FetchContent_MakeAvailable(abseil)
endif()

if(OBL_WITH_MIMALLOC)
  set(MI_BUILD_TESTS OFF)
  set(MI_BUILD_STATIC OFF)
  set(MI_BUILD_OBJECT OFF)
  FetchContent_Declare(mimalloc
    GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
    GIT_TAG v3.5.4
    GIT_SHALLOW TRUE
    SYSTEM)
  FetchContent_MakeAvailable(mimalloc)
  set_target_properties(mimalloc PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()

find_package(ZLIB)

add_library(obl INTERFACE)
target_include_directories(obl INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(obl INTERFACE -Wall -Wextra -Wpedantic)
if(OBL_NATIVE)
  target_compile_options(obl INTERFACE -march=native)
endif()
if(OBL_SANITIZE)
  target_compile_options(obl INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(obl INTERFACE -fsanitize=address,undefined)
endif()
if(OBL_WITH_UNORDERED_DENSE)
  target_compile_definitions(obl INTERFACE OBL_HAVE_UNORDERED_DENSE=1)
  target_link_libraries(obl INTERFACE unordered_dense::unordered_dense)
endif()
if(OBL_WITH_ABSEIL)
  target_compile_definitions(obl INTERFACE OBL_HAVE_ABSEIL=1)
  target_link_libraries(obl INTERFACE absl::btree absl::flat_hash_map)
endif()
if(ZLIB_FOUND)
  target_compile_definitions(obl INTERFACE OBL_HAVE_ZLIB=1)
  target_link_libraries(obl INTERFACE ZLIB::ZLIB)
endif()

add_executable(obl_gen tools/gen.cpp)
target_link_libraries(obl_gen PRIVATE obl)

add_executable(obl_dump tools/dump.cpp)
target_link_libraries(obl_dump PRIVATE obl)

add_executable(obl_itch tools/itch.cpp)
target_link_libraries(obl_itch PRIVATE obl)

add_executable(obl_burst bench/burst.cpp)
target_link_libraries(obl_burst PRIVATE obl)

add_executable(obl_bench bench/bench.cpp)
target_include_directories(obl_bench PRIVATE tools)
target_link_libraries(obl_bench PRIVATE obl)

enable_testing()
foreach(t test_xdp test_book test_itch test_view)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE obl)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
