cmake_minimum_required(VERSION 3.20)
project(mcc C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(MCC_TSAN "用线程竞争检测器编译" OFF)
option(MCC_ASAN "用地址检测器编译" OFF)

add_compile_definitions(_GNU_SOURCE)
if(MINGW)
    # mingw 默认的 msvcrt printf 不支持 %zu 等格式
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()
add_compile_options(-Wall -Wextra -Wno-misleading-indentation)
set(CMAKE_C_FLAGS_RELEASE "-O3 -march=native -g")
if(MCC_TSAN)
    add_compile_options(-fsanitize=thread -O1 -g)
    add_link_options(-fsanitize=thread)
endif()
if(MCC_ASAN)
    add_compile_options(-fsanitize=address,undefined -O1 -g -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

# ---- Box3D（MIT，已改造：体素形状、逐接触片材质、通用六自由度关节）----
file(GLOB BOX3D_SRC third_party/box3d/src/*.c)
add_library(box3d STATIC ${BOX3D_SRC})
set_target_properties(box3d PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED YES C_EXTENSIONS YES)
target_include_directories(box3d PUBLIC third_party/box3d/include PRIVATE third_party/box3d/src)
# 每个有子世界的区域一个 Box3D 世界，默认上限 128 不够
target_compile_definitions(box3d PUBLIC B3_MAX_WORLDS=4096)
target_compile_options(box3d PRIVATE -w -ffp-contract=off)
target_link_libraries(box3d PUBLIC Threads::Threads m)

file(GLOB_RECURSE MCC_CORE_SRC src/core/*.c src/region/*.c src/world/*.c src/create/*.c src/phys/*.c)
add_library(mcc_core STATIC ${MCC_CORE_SRC})
target_link_libraries(mcc_core PUBLIC box3d Threads::Threads m)

# 带简易测试客户端（浏览器）的调试服务端
add_executable(mcc_debug src/debug/main.c src/debug/viewer.c)
target_link_libraries(mcc_debug mcc_core)
if(WIN32)
    target_link_libraries(mcc_debug ws2_32 winmm)
endif()
target_compile_definitions(mcc_debug PRIVATE MCC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

enable_testing()
file(GLOB MCC_TESTS tests/test_*.c)
foreach(t ${MCC_TESTS})
    get_filename_component(name ${t} NAME_WE)
    add_executable(${name} ${t})
    target_link_libraries(${name} mcc_core)
    target_compile_definitions(${name} PRIVATE MCC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME ${name} COMMAND ${name})
endforeach()
