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
if(ZLIB_FOUND)
  target_compile_definitions(obl INTERFACE OBL_HAVE_ZLIB=1)
  target_link_libraries(obl INTERFACE ZLIB::ZLIB)
endif()

add_executable(obl_gen tools/gen.cpp)
target_link_libraries(obl_gen PRIVATE obl)

add_executable(obl_dump tools/dump.cpp)
target_link_libraries(obl_dump PRIVATE obl)

add_executable(obl_bench bench/bench.cpp)
target_include_directories(obl_bench PRIVATE tools)
target_link_libraries(obl_bench PRIVATE obl)

enable_testing()
foreach(t test_xdp test_book)
  add_executable(${t} tests/${t}.cpp)
  target_link_libraries(${t} PRIVATE obl)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
