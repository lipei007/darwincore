#
# DarwinCore Network 测试
#
# 功能说明：
#   DarwinCore Network 模块的集成测试（直接编译源码）
#
# 作者: DarwinCore Network 团队
# 日期: 2026

cmake_minimum_required(VERSION 3.16)
project(darwincore_network_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 获取父目录路径
get_filename_component(PARENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# 包含头文件路径
include_directories(
    ${PARENT_DIR}/include
)

# Network 模块源文件
set(NETWORK_SOURCES
    ${PARENT_DIR}/src/darwincore/network/acceptor.cpp
    ${PARENT_DIR}/src/darwincore/network/client.cpp
    ${PARENT_DIR}/src/darwincore/network/io_monitor.cpp
    ${PARENT_DIR}/src/darwincore/network/reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/server.cpp
    ${PARENT_DIR}/src/darwincore/network/socket_helper.cpp
    ${PARENT_DIR}/src/darwincore/network/worker_pool.cpp
)

# ==================== 测试 1: 基本通信测试 ====================
add_executable(server_client_test
    server_client_test.cpp
    ${NETWORK_SOURCES}
)
target_compile_options(server_client_test PRIVATE -g -O0)

# ==================== 测试 2: kqueue 核心机制测试 ====================
add_executable(test_kqueue_core
    test_kqueue_core.cc
)
target_compile_options(test_kqueue_core PRIVATE -g -O0)

# ==================== 测试 3: 高并发压力测试 ====================
add_executable(test_stress_concurrency
    test_stress_concurrency.cc
    ${NETWORK_SOURCES}
)
target_compile_options(test_stress_concurrency PRIVATE -O2)  # 压力测试使用优化

# ==================== 测试目标 ====================
# 自定义测试目标，支持运行不同类型的测试
add_custom_target(test_small
    COMMAND test_stress_concurrency small
    DEPENDS test_stress_concurrency
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running small-scale stress test (100 connections)"
)

add_custom_target(test_medium
    COMMAND test_stress_concurrency medium
    DEPENDS test_stress_concurrency
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running medium-scale stress test (1000 connections)"
)

add_custom_target(test_large
    COMMAND test_stress_concurrency large
    DEPENDS test_stress_concurrency
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running large-scale stress test (10000 connections)"
)

add_custom_target(test_extreme
    COMMAND test_stress_concurrency extreme
    DEPENDS test_stress_concurrency
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running extreme-scale stress test (50000 connections)"
)

# kqueue 核心测试
add_custom_target(test_kqueue
    COMMAND test_kqueue_core
    DEPENDS test_kqueue_core
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running kqueue core mechanism tests"
)

# ==================== 测试 4: SIGPIPE 处理测试 ====================
add_executable(test_sigpipe
    test_sigpipe.cc
    ${NETWORK_SOURCES}
)
target_compile_options(test_sigpipe PRIVATE -g -O0)

# SIGPIPE 测试
add_custom_target(test_sigpipe_run
    COMMAND test_sigpipe
    DEPENDS test_sigpipe
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running SIGPIPE handling test"
)

# 综合测试
add_custom_target(test_all
    COMMAND test_kqueue_core
    COMMAND server_client_test all
    COMMAND test_stress_concurrency small
    DEPENDS test_kqueue_core server_client_test test_stress_concurrency
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all tests"
)
