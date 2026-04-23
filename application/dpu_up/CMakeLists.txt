cmake_minimum_required(VERSION 3.22)

project(dpu_up VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DOCA_ROOT "/opt/mellanox/doca" CACHE PATH "Path to DOCA installation root")

find_package(PkgConfig REQUIRED)
pkg_check_modules(DOCA REQUIRED IMPORTED_TARGET
    doca-common
    doca-dma
)

add_library(dpu_up_runtime STATIC
    ${CMAKE_CURRENT_LIST_DIR}/src/runtime.cpp
    ${DOCA_ROOT}/samples/doca_common/pe_common.c
    ${DOCA_ROOT}/samples/common.c
    ${DOCA_ROOT}/applications/common/utils.c
)

target_compile_definitions(dpu_up_runtime PRIVATE DOCA_ALLOW_EXPERIMENTAL_API)
target_include_directories(dpu_up_runtime PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${DOCA_ROOT}/include
    ${DOCA_ROOT}
    ${DOCA_ROOT}/samples
    ${DOCA_ROOT}/samples/doca_common
    ${DOCA_ROOT}/applications/common
)
target_compile_options(dpu_up_runtime PRIVATE -Wno-missing-braces -Wno-deprecated-declarations)
target_link_libraries(dpu_up_runtime PUBLIC PkgConfig::DOCA pthread)

add_executable(up_fg ${CMAKE_CURRENT_LIST_DIR}/main/up_fg.cpp)
target_include_directories(up_fg PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(up_fg PRIVATE dpu_up_runtime pthread)

add_executable(up_bg ${CMAKE_CURRENT_LIST_DIR}/main/up_bg.cpp)
target_include_directories(up_bg PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(up_bg PRIVATE dpu_up_runtime pthread)

add_executable(up_fg_race ${CMAKE_CURRENT_LIST_DIR}/main/up_fg_race.cpp)
target_include_directories(up_fg_race PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(up_fg_race PRIVATE dpu_up_runtime pthread)

add_executable(up_bg_race ${CMAKE_CURRENT_LIST_DIR}/main/up_bg_race.cpp)
target_include_directories(up_bg_race PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include)
target_link_libraries(up_bg_race PRIVATE dpu_up_runtime pthread)

add_executable(up_fg_demo ${CMAKE_CURRENT_LIST_DIR}/main/up_fg_demo.cpp)
target_include_directories(up_fg_demo PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    /home/cdf/xsched/include
    /home/cdf/xsched/preempt/include
    /home/cdf/xsched/platforms/doca28/hal/include
    /home/cdf/xsched/utils/include
    /home/cdf/xsched/protocol/include
    /home/cdf/xsched/sched/include
)
target_link_libraries(up_fg_demo PRIVATE
    dpu_up_runtime
    /home/cdf/xsched/build/preempt/libpreempt.so
    /home/cdf/xsched/build/platforms/doca28/libhaldoca28.so
    pthread
)

add_executable(up_bg_demo ${CMAKE_CURRENT_LIST_DIR}/main/up_bg_demo.cpp)
target_include_directories(up_bg_demo PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
    /home/cdf/xsched/include
    /home/cdf/xsched/preempt/include
    /home/cdf/xsched/platforms/doca28/hal/include
    /home/cdf/xsched/utils/include
    /home/cdf/xsched/protocol/include
    /home/cdf/xsched/sched/include
)
target_link_libraries(up_bg_demo PRIVATE
    dpu_up_runtime
    /home/cdf/xsched/build/preempt/libpreempt.so
    /home/cdf/xsched/build/platforms/doca28/libhaldoca28.so
    pthread
)
