cmake_minimum_required(VERSION 3.12)

project(gpu_up VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS_RELEASE   "$ENV{CFLAGS}   -O3 -DRELEASE_MODE")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -O3 -DRELEASE_MODE")
set(CMAKE_C_FLAGS_DEBUG     "$ENV{CFLAGS}   -O0 -g2 -ggdb -DDEBUG_MODE")
set(CMAKE_CXX_FLAGS_DEBUG   "$ENV{CXXFLAGS} -O0 -g2 -ggdb -DDEBUG_MODE")

add_subdirectory(trt)

find_package(Threads REQUIRED)

file(GLOB_RECURSE MAIN_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/main/*.c
    ${CMAKE_CURRENT_LIST_DIR}/main/*.cpp
)

add_library(gpu_up_utils INTERFACE)
target_include_directories(gpu_up_utils INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/include
)
target_link_libraries(gpu_up_utils INTERFACE Threads::Threads)

foreach(MAIN_SRC ${MAIN_SRCS})
    get_filename_component(MAIN_TARGET ${MAIN_SRC} NAME_WE)
    add_executable(${MAIN_TARGET} ${MAIN_SRC})
    target_link_libraries(${MAIN_TARGET} PRIVATE
        trt_infer
        gpu_up_utils
    )
endforeach()
