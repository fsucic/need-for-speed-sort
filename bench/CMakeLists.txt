cmake_minimum_required(VERSION 3.20)
project(nfsbench CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Paper: -O3 -march=native -DNDEBUG. Apple clang takes -mcpu=native on arm64.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(native_flag -mcpu=native)
else()
  set(native_flag -march=native)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O3 ${native_flag} -DNDEBUG")

find_package(Boost 1.80 REQUIRED CONFIG)
add_executable(nfsbench bench.cpp)
add_executable(fran_f64 fran_f64.cpp)
target_include_directories(nfsbench PRIVATE
  ${CMAKE_SOURCE_DIR}/..
  ${CMAKE_SOURCE_DIR}/third_party/ips4o/include
  ${CMAKE_SOURCE_DIR}/third_party/ips2ra/include
  ${CMAKE_SOURCE_DIR}/third_party/pdqsort
  ${CMAKE_SOURCE_DIR}/third_party/bls/include)
find_package(TBB REQUIRED)
target_link_libraries(nfsbench PRIVATE Boost::headers TBB::tbb)

foreach(variant IN ITEMS radix classic)
  string(TOUPPER "${variant}" upper)
  set(NFS_${upper}_REV "" CACHE STRING "Local Git revision of the optional ${variant} sort")
  if(NFS_${upper}_REV)
    find_package(Git REQUIRED)
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" show "${NFS_${upper}_REV}:nfss.h"
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE header
      COMMAND_ERROR_IS_FATAL ANY)
    string(REPLACE "namespace nfs {" "namespace nfs_${variant} {" header "${header}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/nfs_${variant}.h" "${header}")
    target_include_directories(nfsbench PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(nfsbench PRIVATE NFS_BENCH_${upper})
    if(variant STREQUAL "radix")
      target_include_directories(fran_f64 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
      target_compile_definitions(fran_f64 PRIVATE NFS_BENCH_RADIX)
    endif()
  endif()
endforeach()
