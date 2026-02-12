cmake_minimum_required(VERSION 3.16)
project(darwincore_network_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()

get_filename_component(PARENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

include_directories(
    ${PARENT_DIR}/include
    ${PARENT_DIR}/src/darwincore/network
)

set(NETWORK_SOURCES
    ${PARENT_DIR}/src/darwincore/network/acceptor.cpp
    ${PARENT_DIR}/src/darwincore/network/client.cpp
    ${PARENT_DIR}/src/darwincore/network/client_reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/connection_id_generator.cpp
    ${PARENT_DIR}/src/darwincore/network/io_monitor.cpp
    ${PARENT_DIR}/src/darwincore/network/protocol.cpp
    ${PARENT_DIR}/src/darwincore/network/reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/send_buffer.cpp
    ${PARENT_DIR}/src/darwincore/network/server.cpp
    ${PARENT_DIR}/src/darwincore/network/socket_helper.cpp
    ${PARENT_DIR}/src/darwincore/network/worker_pool.cpp
)

add_executable(test_server_statistics
    test_server_statistics.cpp
    ${PARENT_DIR}/src/darwincore/network/server.cpp
    ${PARENT_DIR}/src/darwincore/network/acceptor.cpp
    ${PARENT_DIR}/src/darwincore/network/reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/worker_pool.cpp
    ${PARENT_DIR}/src/darwincore/network/io_monitor.cpp
    ${PARENT_DIR}/src/darwincore/network/send_buffer.cpp
    ${PARENT_DIR}/src/darwincore/network/socket_helper.cpp
    ${PARENT_DIR}/src/darwincore/network/connection_id_generator.cpp
)
target_compile_options(test_server_statistics PRIVATE -g -O0)

add_test(NAME test_server_statistics COMMAND test_server_statistics)

add_executable(test_worker_pool_dispatch
    test_worker_pool_dispatch.cpp
    ${PARENT_DIR}/src/darwincore/network/worker_pool.cpp
)
target_compile_options(test_worker_pool_dispatch PRIVATE -g -O0)
add_test(NAME test_worker_pool_dispatch COMMAND test_worker_pool_dispatch)

add_executable(test_reactor_wakeup
    test_reactor_wakeup.cpp
    ${PARENT_DIR}/src/darwincore/network/reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/worker_pool.cpp
    ${PARENT_DIR}/src/darwincore/network/io_monitor.cpp
    ${PARENT_DIR}/src/darwincore/network/send_buffer.cpp
    ${PARENT_DIR}/src/darwincore/network/socket_helper.cpp
    ${PARENT_DIR}/src/darwincore/network/connection_id_generator.cpp
)
target_compile_options(test_reactor_wakeup PRIVATE -g -O0)
add_test(NAME test_reactor_wakeup COMMAND test_reactor_wakeup)

add_executable(test_server_options
    test_server_options.cpp
    ${PARENT_DIR}/src/darwincore/network/server.cpp
    ${PARENT_DIR}/src/darwincore/network/acceptor.cpp
    ${PARENT_DIR}/src/darwincore/network/reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/worker_pool.cpp
    ${PARENT_DIR}/src/darwincore/network/io_monitor.cpp
    ${PARENT_DIR}/src/darwincore/network/send_buffer.cpp
    ${PARENT_DIR}/src/darwincore/network/socket_helper.cpp
    ${PARENT_DIR}/src/darwincore/network/connection_id_generator.cpp
)
target_compile_options(test_server_options PRIVATE -g -O0)
add_test(NAME test_server_options COMMAND test_server_options)

add_executable(test_reactor_operation_stats
    test_reactor_operation_stats.cpp
    ${PARENT_DIR}/src/darwincore/network/reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/worker_pool.cpp
    ${PARENT_DIR}/src/darwincore/network/io_monitor.cpp
    ${PARENT_DIR}/src/darwincore/network/send_buffer.cpp
    ${PARENT_DIR}/src/darwincore/network/socket_helper.cpp
    ${PARENT_DIR}/src/darwincore/network/connection_id_generator.cpp
)
target_compile_options(test_reactor_operation_stats PRIVATE -g -O0)
add_test(NAME test_reactor_operation_stats COMMAND test_reactor_operation_stats)

add_executable(test_server_high_concurrency
    test_server_high_concurrency.cpp
    ${PARENT_DIR}/src/darwincore/network/acceptor.cpp
    ${PARENT_DIR}/src/darwincore/network/client.cpp
    ${PARENT_DIR}/src/darwincore/network/client_reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/connection_id_generator.cpp
    ${PARENT_DIR}/src/darwincore/network/io_monitor.cpp
    ${PARENT_DIR}/src/darwincore/network/reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/send_buffer.cpp
    ${PARENT_DIR}/src/darwincore/network/server.cpp
    ${PARENT_DIR}/src/darwincore/network/socket_helper.cpp
    ${PARENT_DIR}/src/darwincore/network/worker_pool.cpp
)
target_compile_options(test_server_high_concurrency PRIVATE -g -O0)
add_test(NAME test_server_high_concurrency COMMAND test_server_high_concurrency)

add_executable(test_server_soak
    test_server_soak.cpp
    ${PARENT_DIR}/src/darwincore/network/acceptor.cpp
    ${PARENT_DIR}/src/darwincore/network/client.cpp
    ${PARENT_DIR}/src/darwincore/network/client_reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/connection_id_generator.cpp
    ${PARENT_DIR}/src/darwincore/network/io_monitor.cpp
    ${PARENT_DIR}/src/darwincore/network/reactor.cpp
    ${PARENT_DIR}/src/darwincore/network/send_buffer.cpp
    ${PARENT_DIR}/src/darwincore/network/server.cpp
    ${PARENT_DIR}/src/darwincore/network/socket_helper.cpp
    ${PARENT_DIR}/src/darwincore/network/worker_pool.cpp
)
target_compile_options(test_server_soak PRIVATE -g -O0)
add_test(NAME test_server_soak COMMAND test_server_soak)

add_executable(protocol_echo_server
    protocol_echo_server.cpp
    ${NETWORK_SOURCES}
)
target_compile_options(protocol_echo_server PRIVATE -g -O0)

add_executable(protocol_echo_client
    protocol_echo_client.cpp
    ${NETWORK_SOURCES}
)
target_compile_options(protocol_echo_client PRIVATE -g -O0)
