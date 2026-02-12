# ========================================
# MyLib - C++ 基础库项目
# ========================================
# 作者: Darwin Core
# 描述: 提供网络通信、文件操作、日志系统、线程管理等基础功能的 C++ 库
# 版本: 1.0.0
# ========================================

# 设置 CMake 最低版本要求为 3.20
cmake_minimum_required(VERSION 3.20)

# 项目名称: MyLib
project(MyLib)
enable_testing()

# 设置 C++ 标准为 C++17
SET(CMAKE_CXX_STANDARD 17)

# 版本号设置
set(MyLib_MAJOR_VERSION 1)
set(MyLib_MINOR_VERSION 0)
set(MyLib_PATCH_VERSION 0)

# 设置目标输出目录（可选）
#SET(EXECUTABLE_OUTPUT_PATH "${PROJECT_SOURCE_DIR}/build/export/bin")
#SET(LIBRARY_OUTPUT_PATH "${PROJECT_SOURCE_DIR}/build/export/lib")

# 输出项目信息
MESSAGE("project source dir:${PROJECT_SOURCE_DIR} project name:${PROJECT_NAME}")

# 设置全局头文件搜索路径
set(PROJECT_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)
include_directories(${PROJECT_INCLUDE_DIR})

# 根据操作系统类型输出提示信息
if (APPLE)
    MESSAGE("This is apple")
elseif (UNIX)
    MESSAGE("This is unix")
else()
    MESSAGE("This is windows")
endif ()

# 添加 src 子目录，包含项目的源代码
add_subdirectory(src)

# 添加 test 子目录，包含测试代码
add_subdirectory(test)
