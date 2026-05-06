cmake_minimum_required(VERSION 3.12)

project(gpu_transparent_up VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS_RELEASE   "$ENV{CFLAGS}   -O3 -DRELEASE_MODE")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -DRELEASE_MODE")
set(CMAKE_C_FLAGS_DEBUG     "$ENV{CFLAGS}   -O0 -g2 -ggdb -DDEBUG_MODE")
set(CMAKE_CXX_FLAGS_DEBUG   "$ENV{CXXFLAGS} -O0 -g2 -ggdb -DDEBUG_MODE")

add_subdirectory(trt)

find_package(Threads REQUIRED)

add_library(gpu_transparent_up_utils INTERFACE)
target_include_directories(gpu_transparent_up_utils INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/include
)
target_link_libraries(gpu_transparent_up_utils INTERFACE Threads::Threads)

add_executable(up_loop main/up_loop.cpp)
target_link_libraries(up_loop PRIVATE
    trt_infer
    gpu_transparent_up_utils
)
