cmake_minimum_required(VERSION 3.15)

# 项目名称
project(FlexibleExactSynthesis VERSION 0.1 LANGUAGES CXX)

# C++ 标准: C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# =============================================================
#  Dependency roots
# =============================================================

set(Z3_ROOT "" CACHE PATH "Root directory of a Z3 source/build/install tree")
set(KISSAT_ROOT "" CACHE PATH "Root directory of a Kissat source/build/install tree")

if(NOT Z3_ROOT AND DEFINED ENV{Z3_ROOT})
    set(Z3_ROOT "$ENV{Z3_ROOT}" CACHE PATH "Root directory of a Z3 source/build/install tree" FORCE)
endif()

if(NOT KISSAT_ROOT AND DEFINED ENV{KISSAT_ROOT})
    set(KISSAT_ROOT "$ENV{KISSAT_ROOT}" CACHE PATH "Root directory of a Kissat source/build/install tree" FORCE)
endif()

# =============================================================
#  1. 查找并配置 Z3 (Z3 Configuration)
# =============================================================
message(STATUS "--- Configuring Z3 ---")
if(Z3_ROOT)
    message(STATUS "Searching in root: ${Z3_ROOT}")
else()
    message(STATUS "Z3_ROOT not set; searching default compiler/system paths")
endif()

# 1.1 查找头文件 (z3++.h)
# 搜索顺序: ROOT/src/api/c++, ROOT/src/api, ROOT/include
find_path(Z3_CXX_HEADER_DIR NAMES z3++.h
    HINTS ${Z3_ROOT}
    PATH_SUFFIXES src/api/c++ src/api include
)
# 查找 z3.h (有时候它在另一个目录，必须显式包含)
find_path(Z3_C_HEADER_DIR NAMES z3.h
    HINTS ${Z3_ROOT}
    PATH_SUFFIXES src/api include
)

# 1.2 查找库文件 (libz3.so)
# 搜索顺序: ROOT/build, ROOT/lib, ROOT/bin
find_library(Z3_LIB NAMES z3 libz3
    HINTS ${Z3_ROOT}
    PATH_SUFFIXES build lib bin
)

if (Z3_CXX_HEADER_DIR AND Z3_C_HEADER_DIR AND Z3_LIB)
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
    message(FATAL_ERROR "Could NOT find Z3. Configure with -DZ3_ROOT=/path/to/z3 or install Z3 in a default search path.")
endif()

# =============================================================
#  2. 查找并配置 Kissat (Kissat Configuration)
# =============================================================
message(STATUS "--- Configuring Kissat ---")
if(KISSAT_ROOT)
    message(STATUS "Searching in root: ${KISSAT_ROOT}")
else()
    message(STATUS "KISSAT_ROOT not set; searching default compiler/system paths")
endif()

# 2.1 查找头文件 (kissat.h)
find_path(KISSAT_INCLUDE_DIR NAMES kissat.h
    HINTS ${KISSAT_ROOT}
    PATH_SUFFIXES src include .
)

# 2.2 查找库文件 (libkissat.a)
find_library(KISSAT_LIB NAMES kissat libkissat
    HINTS ${KISSAT_ROOT}
    PATH_SUFFIXES build lib .
)

if (KISSAT_INCLUDE_DIR AND KISSAT_LIB)
    message(STATUS "  Found Kissat Header: ${KISSAT_INCLUDE_DIR}")
    message(STATUS "  Found Kissat Library: ${KISSAT_LIB}")
    add_compile_definitions(HAS_KISSAT) # 定义宏
else()
    if(KISSAT_ROOT)
        message(FATAL_ERROR "Could NOT find Kissat in ${KISSAT_ROOT}. Check -DKISSAT_ROOT or leave it empty to build without Kissat.")
    endif()
    message(WARNING "Could NOT find Kissat. Continuing without Kissat-backed SAT solver support.")
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

enable_testing()

# 4.1 主程序
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(FILTER SOURCES EXCLUDE REGEX ".*/src/tests/.*\\.cpp$")
add_executable(fes_app ${SOURCES})
target_compile_definitions(fes_app PRIVATE FES_PROJECT_ROOT="${PROJECT_SOURCE_DIR}")
target_link_libraries(fes_app PRIVATE Z3::Z3)
if(KISSAT_LIB)
    target_link_libraries(fes_app PRIVATE ${KISSAT_LIB})
endif()

# 4.2 NPN 单元测试
add_executable(test_npn
    src/tests/test_npn.cpp
    src/core/NpnTransform.cpp
)
add_test(NAME test_npn COMMAND test_npn)

add_executable(test_activity_patterns
    src/tests/test_activity_patterns.cpp
    src/flow/ActivityPatternGenerator.cpp
)
add_test(NAME test_activity_patterns COMMAND test_activity_patterns)

add_executable(test_generate_cli
    src/tests/test_generate_cli.cpp
    src/app/GenerateCli.cpp
    src/app/AppConfig.cpp
    src/flow/ActivityPatternGenerator.cpp
    src/flow/RunManifest.cpp
)
add_test(NAME test_generate_cli COMMAND test_generate_cli)

add_executable(test_synthesis_library_loader
    src/tests/test_synthesis_library_loader.cpp
    src/utils/SynthesisLibraryLoader.cpp
)
add_test(NAME test_synthesis_library_loader COMMAND test_synthesis_library_loader)

add_executable(test_run_manifest
    src/tests/test_run_manifest.cpp
    src/flow/RunManifest.cpp
)
add_test(NAME test_run_manifest COMMAND test_run_manifest)
