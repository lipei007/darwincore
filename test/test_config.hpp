//
// DarwinCore Network 测试配置
//
// 功能说明：
//   提供测试配置参数，支持不同规模的测试
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#ifndef DARWINCORE_TEST_CONFIG_HPP
#define DARWINCORE_TEST_CONFIG_HPP

#include <cstdint>
#include <string>

namespace darwincore {
namespace test {

// 测试规模配置
struct TestScale {
  const char* name;
  int num_connections;
  int num_messages;
  int message_size;
  int num_reactor_threads;
  int num_worker_threads;
  int port;
  const char* description;
};

// 预定义测试规模
namespace scale {

// 小规模测试 - 快速验证
constexpr TestScale kSmall = {
  "small",            // name
  100,                // num_connections
  10,                 // num_messages
  1024,               // message_size (1KB)
  2,                  // num_reactor_threads
  2,                  // num_worker_threads
  9999,               // port
  "小规模测试 - 快速验证功能正确性"
};

// 中等规模测试 - 常规性能测试
constexpr TestScale kMedium = {
  "medium",           // name
  1000,               // num_connections
  100,                // num_messages
  4096,               // message_size (4KB)
  4,                  // num_reactor_threads
  4,                  // num_worker_threads
  9998,               // port
  "中等规模测试 - 常规性能测试"
};

// 大规模测试 - 压力测试
constexpr TestScale kLarge = {
  "large",            // name
  10000,              // num_connections
  1000,               // num_messages
  8192,               // message_size (8KB)
  8,                  // num_reactor_threads
  8,                  // num_worker_threads
  9997,               // port
  "大规模测试 - 压力测试"
};

// 极限规模测试 - 性能极限
constexpr TestScale kExtreme = {
  "extreme",          // name
  50000,              // num_connections
  100,                // num_messages
  1024,               // message_size (1KB)
  16,                 // num_reactor_threads
  16,                 // num_worker_threads
  9996,               // port
  "极限规模测试 - 性能极限测试"
};

} // namespace scale

// 性能基准配置
struct PerformanceBaseline {
  uint64_t min_qps;              // 最低 QPS
  uint64_t min_throughput_mb;    // 最低吞吐量 (MB/s)
  double max_latency_ms;         // 最大延迟 (ms)
  double min_success_rate;       // 最低成功率 (0-1)
};

// 预定义性能基准
namespace baseline {

// 小规模基准
constexpr PerformanceBaseline kSmall = {
  50000,              // min_qps: 50K msg/s
  50,                 // min_throughput_mb: 50 MB/s
  2.0,                // max_latency_ms: 2ms
  0.99                // min_success_rate: 99%
};

// 中等规模基准
constexpr PerformanceBaseline kMedium = {
  100000,             // min_qps: 100K msg/s
  200,                // min_throughput_mb: 200 MB/s
  5.0,                // max_latency_ms: 5ms
  0.98                // min_success_rate: 98%
};

// 大规模基准
constexpr PerformanceBaseline kLarge = {
  200000,             // min_qps: 200K msg/s
  500,                // min_throughput_mb: 500 MB/s
  10.0,               // max_latency_ms: 10ms
  0.95                // min_success_rate: 95%
};

// 极限规模基准
constexpr PerformanceBaseline kExtreme = {
  300000,             // min_qps: 300K msg/s
  1000,               // min_throughput_mb: 1000 MB/s
  20.0,               // max_latency_ms: 20ms
  0.90                // min_success_rate: 90%
};

} // namespace baseline

// 测试超时配置
struct TimeoutConfig {
  int connection_timeout_ms;     // 连接超时
  int message_timeout_ms;        // 消息超时
  int test_timeout_sec;          // 整体测试超时
};

// 默认超时配置
constexpr TimeoutConfig kDefaultTimeout = {
  5000,               // connection_timeout_ms: 5s
  30000,              // message_timeout_ms: 30s
  300                 // test_timeout_sec: 5min
};

// 系统限制配置
struct SystemLimits {
  int min_fd_limit;              // 最低文件描述符限制
  int min_process_limit;         // 最低进程限制
  int recommended_fd_limit;      // 推荐文件描述符限制
};

constexpr SystemLimits kSystemLimits = {
  4096,               // min_fd_limit
  1024,               // min_process_limit
  65536               // recommended_fd_limit
};

} // namespace test
} // namespace darwincore

#endif  // DARWINCORE_TEST_CONFIG_HPP
