cmake_minimum_required(VERSION 3.15)

# 项目名称
project(FlexibleExactSynthesis VERSION 0.1 LANGUAGES CXX)

# C++ 标准: C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================
#  用户配置区 (USER CONFIGURATION)
#  请在这里填入你的 Z3 和 Kissat 源码/安装的根目录
# =============================================================

# TODO: 修改这里为你的 Z3 根目录 (例如: /home/yhs_joker/tools/z3)
set(USER_Z3_ROOT "/home/yhs_joker/softwares/z3") 

# TODO: 修改这里为你的 Kissat 根目录 (例如: /home/yhs_joker/tools/kissat)
set(USER_KISSAT_ROOT "/home/yhs_joker/softwares/kissat")

# =============================================================
#  1. 查找并配置 Z3 (Z3 Configuration)
# =============================================================
message(STATUS "--- Configuring Z3 ---")
message(STATUS "Searching in root: ${USER_Z3_ROOT}")

# 1.1 查找头文件 (z3++.h)
# 搜索顺序: ROOT/src/api/c++, ROOT/src/api, ROOT/include
find_path(Z3_CXX_HEADER_DIR NAMES z3++.h
    HINTS ${USER_Z3_ROOT}
    PATH_SUFFIXES src/api/c++ src/api include
    NO_DEFAULT_PATH
)
# 查找 z3.h (有时候它在另一个目录，必须显式包含)
find_path(Z3_C_HEADER_DIR NAMES z3.h
    HINTS ${USER_Z3_ROOT}
    PATH_SUFFIXES src/api include
    NO_DEFAULT_PATH
)

# 1.2 查找库文件 (libz3.so)
# 搜索顺序: ROOT/build, ROOT/lib, ROOT/bin
find_library(Z3_LIB NAMES z3 libz3
    HINTS ${USER_Z3_ROOT}
    PATH_SUFFIXES build lib bin
    NO_DEFAULT_PATH
)

if (Z3_CXX_HEADER_DIR AND Z3_LIB)
    message(STATUS "  Found Z3 Header: ${Z3_CXX_HEADER_DIR}")
    message(STATUS "  Found Z3 Library: ${Z3_LIB}")

    # 创建伪目标 Z3::Z3
    add_library(Z3::Z3 UNKNOWN IMPORTED)
    set_target_properties(Z3::Z3 PROPERTIES
        IMPORTED_LOCATION "${Z3_LIB}"
        # 包含 C++ 和 C 的头文件路径
        INTERFACE_INCLUDE_DIRECTORIES "${Z3_CXX_HEADER_DIR};${Z3_C_HEADER_DIR}"
    )
else()
    message(FATAL_ERROR "Could NOT find Z3 in ${USER_Z3_ROOT}. Please check path and build status.")
endif()

# =============================================================
#  2. 查找并配置 Kissat (Kissat Configuration)
# =============================================================
message(STATUS "--- Configuring Kissat ---")
message(STATUS "Searching in root: ${USER_KISSAT_ROOT}")

# 2.1 查找头文件 (kissat.h)
find_path(KISSAT_INCLUDE_DIR NAMES kissat.h
    HINTS ${USER_KISSAT_ROOT}
    PATH_SUFFIXES src include .
    NO_DEFAULT_PATH
)

# 2.2 查找库文件 (libkissat.a)
find_library(KISSAT_LIB NAMES kissat libkissat
    HINTS ${USER_KISSAT_ROOT}
    PATH_SUFFIXES build lib .
    NO_DEFAULT_PATH
)

if (KISSAT_INCLUDE_DIR AND KISSAT_LIB)
    message(STATUS "  Found Kissat Header: ${KISSAT_INCLUDE_DIR}")
    message(STATUS "  Found Kissat Library: ${KISSAT_LIB}")
    add_compile_definitions(HAS_KISSAT) # 定义宏
else()
    message(WARNING "Could NOT find Kissat in ${USER_KISSAT_ROOT}. Continuing without SAT solver support.")
endif()

# =============================================================
#  3. 项目通用设置 (Project Settings)
# =============================================================

# 包含项目自身的头文件
include_directories(include)

# 添加 Kissat 头文件路径 (如果找到)
if(KISSAT_INCLUDE_DIR)
    include_directories(${KISSAT_INCLUDE_DIR})
endif()

# =============================================================
#  4. 构建目标 (Build Targets)
# =============================================================

# 4.1 主程序
file(GLOB_RECURSE SOURCES "src/*.cpp")
add_executable(fes_app ${SOURCES})
target_link_libraries(fes_app PRIVATE Z3::Z3)
if(KISSAT_LIB)
    target_link_libraries(fes_app PRIVATE ${KISSAT_LIB})
endif()

# # 4.2 环境测试程序 (TestEnv)
# add_executable(test_env tests/TestEnv.cpp)
# target_link_libraries(test_env PRIVATE Z3::Z3)
# if(KISSAT_LIB)
#     target_link_libraries(test_env PRIVATE ${KISSAT_LIB})
# endif()