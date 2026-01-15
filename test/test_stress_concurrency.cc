//
// DarwinCore Network 模块 - 高并发压力测试
//
// 测试目标：
//   1. accept 风暴测试（高并发连接）
//   2. 大量并发连接的稳定性
//   3. Reactor 线程池负载均衡
//   4. 内存与资源泄漏检测
//   5. 性能极限测试（QPS、延迟、并发连接数）
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <memory>
#include <cstring>
#include <random>
#include <iomanip>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/resource.h>

#include <darwincore/network/server.h>
#include <darwincore/network/client.h>

using namespace darwincore::network;

// 测试配置
struct StressTestConfig {
  int num_connections;         // 并发连接数
  int num_messages;            // 每个连接发送的消息数
  int message_size;            // 消息大小（字节）
  int num_reactor_threads;     // Reactor 线程数
  int num_worker_threads;      // Worker 线程数
  bool verbose;                // 详细输出
};

// 性能统计
struct PerformanceStats {
  std::atomic<uint64_t> total_bytes_sent{0};
  std::atomic<uint64_t> total_bytes_received{0};
  std::atomic<uint64_t> total_messages_sent{0};
  std::atomic<uint64_t> total_messages_received{0};
  std::atomic<uint64_t> total_connections{0};
  std::atomic<uint64_t> failed_connections{0};

  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point end_time;

  void Start() {
    start_time = std::chrono::steady_clock::now();
  }

  void Stop() {
    end_time = std::chrono::steady_clock::now();
  }

  double ElapsedSeconds() const {
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time);
    return duration.count() / 1000.0;
  }

  void Print() const {
    double elapsed = ElapsedSeconds();
    uint64_t sent = total_bytes_sent;
    uint64_t received = total_bytes_received;
    uint64_t msg_sent = total_messages_sent;
    uint64_t msg_received = total_messages_received;
    uint64_t conns = total_connections;
    uint64_t failed = failed_connections;

    std::cout << "\n========== 性能统计 ==========" << std::endl;
    std::cout << "测试时长: " << std::fixed << std::setprecision(2) << elapsed << " 秒" << std::endl;
    std::cout << "总连接数: " << conns << std::endl;
    std::cout << "失败连接: " << failed << std::endl;
    std::cout << "发送消息: " << msg_sent << std::endl;
    std::cout << "接收消息: " << msg_received << std::endl;
    std::cout << "发送字节: " << sent << " ("
              << std::fixed << std::setprecision(2) << (sent / 1024.0 / 1024.0) << " MB)" << std::endl;
    std::cout << "接收字节: " << received << " ("
              << std::fixed << std::setprecision(2) << (received / 1024.0 / 1024.0) << " MB)" << std::endl;

    if (elapsed > 0) {
      std::cout << "\n性能指标:" << std::endl;
      std::cout << "QPS: " << static_cast<int>(msg_received / elapsed) << " msg/s" << std::endl;
      std::cout << "吞吐量: " << static_cast<int>(received / elapsed / 1024.0 / 1024.0) << " MB/s" << std::endl;
      std::cout << "平均延迟: " << std::fixed << std::setprecision(3)
                << (elapsed * 1000.0 / msg_received) << " ms/msg" << std::endl;
    }
    std::cout << "==============================\n" << std::endl;
  }
};

//
// 测试 1: Accept 风暴测试
//
bool TestAcceptStorm(const StressTestConfig& config) {
  std::cout << "\n========== 测试 1: Accept 风暴 ==========" << std::endl;
  std::cout << "并发连接数: " << config.num_connections << std::endl;

  std::atomic<bool> server_ready(false);
  std::atomic<bool> test_done(false);
  std::atomic<int> accepted_connections(0);

  Server server;
  PerformanceStats stats;
  stats.Start();

  // 启动 Server
  std::thread server_thread([&]() {
    server.SetOnClientConnected([&](const ConnectionInformation& info) {
      accepted_connections++;
      if (config.verbose && accepted_connections % 100 == 0) {
        std::cout << "[Server] 已接受 " << accepted_connections << " 个连接" << std::endl;
      }
    });

    server.SetOnMessage([&](uint64_t conn_id, const std::vector<uint8_t>& data) {
      stats.total_messages_received++;
      stats.total_bytes_received += data.size();

      // Echo 回去
      server.SendData(conn_id, data.data(), data.size());
    });

    server.SetOnClientDisconnected([](uint64_t conn_id) {
      // 可选：记录断开连接
    });

    server.SetOnConnectionError([](uint64_t conn_id, NetworkError error,
                                   const std::string& message) {
      std::cerr << "[Server] 连接错误: " << static_cast<int>(error)
                << ", " << message << std::endl;
    });

    if (!server.StartIPv4("127.0.0.1", 9999)) {
      std::cerr << "[Server] 启动失败!" << std::endl;
      test_done = true;
      return;
    }

    server_ready = true;
    std::cout << "[Server] 启动成功，监听 127.0.0.1:9999" << std::endl;

    // 等待测试完成
    while (!test_done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.Stop();
    std::cout << "[Server] 已停止" << std::endl;
  });

  // 等待 Server 准备好
  while (!server_ready) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // 创建多个客户端线程
  const int num_client_threads = std::min(10, config.num_connections);
  const int connections_per_thread = config.num_connections / num_client_threads;

  std::vector<std::thread> client_threads;

  for (int t = 0; t < num_client_threads; t++) {
    client_threads.emplace_back([&, t]() {
      int base_index = t * connections_per_thread;
      int end_index = (t == num_client_threads - 1) ?
                      config.num_connections : base_index + connections_per_thread;

      for (int i = base_index; i < end_index; i++) {
        Client client;
        std::atomic<bool> connected(false);
        std::atomic<bool> done(false);

        client.SetOnConnected([&](const ConnectionInformation& info) {
          connected = true;
          stats.total_connections++;

          // 发送测试消息
          std::vector<uint8_t> data(config.message_size, 'A' + (i % 26));
          client.SendData(data.data(), data.size());
          stats.total_messages_sent++;
          stats.total_bytes_sent += data.size();
        });

        client.SetOnMessage([&](const std::vector<uint8_t>& data) {
          stats.total_messages_received++;
          stats.total_bytes_received += data.size();
          done = true;
        });

        client.SetOnError([&](NetworkError error, const std::string& message) {
          if (config.verbose) {
            std::cerr << "[Client] 连接 " << i << " 错误: "
                      << static_cast<int>(error) << std::endl;
          }
          stats.failed_connections++;
          done = true;
        });

        if (!client.ConnectIPv4("127.0.0.1", 9999)) {
          stats.failed_connections++;
          continue;
        }

        // 等待完成
        int wait_count = 0;
        while (!done && wait_count < 50) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          wait_count++;
        }

        client.Disconnect();

        // 短暂延迟，避免过快连接
        if (i % 100 == 0 && i > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      }
    });
  }

  // 等待所有客户端线程完成
  for (auto& t : client_threads) {
    if (t.joinable()) {
      t.join();
    }
  }

  // 等待服务器处理完剩余消息
  std::this_thread::sleep_for(std::chrono::seconds(2));

  test_done = true;
  if (server_thread.joinable()) {
    server_thread.join();
  }

  stats.Stop();
  stats.Print();

  // 验证结果
  bool success = (stats.total_connections == config.num_connections);
  std::cout << "========== Accept 风暴测试 "
            << (success ? "通过" : "失败")
            << " ==========\n" << std::endl;

  return success;
}

//
// 测试 2: 持续吞吐量测试
//
bool TestSustainedThroughput(const StressTestConfig& config) {
  std::cout << "\n========== 测试 2: 持续吞吐量 ==========" << std::endl;
  std::cout << "连接数: " << config.num_connections << std::endl;
  std::cout << "每连接消息数: " << config.num_messages << std::endl;

  std::atomic<bool> server_ready(false);
  std::atomic<bool> test_done(false);
  std::atomic<int> clients_completed(0);

  Server server;
  PerformanceStats stats;
  stats.Start();

  // 启动 Server
  std::thread server_thread([&]() {
    server.SetOnMessage([&](uint64_t conn_id, const std::vector<uint8_t>& data) {
      stats.total_messages_received++;
      stats.total_bytes_received += data.size();

      // Echo 回去
      server.SendData(conn_id, data.data(), data.size());
    });

    server.SetOnConnectionError([](uint64_t conn_id, NetworkError error,
                                   const std::string& message) {
      std::cerr << "[Server] 连接错误: " << static_cast<int>(error) << std::endl;
    });

    if (!server.StartIPv4("127.0.0.1", 9998)) {
      std::cerr << "[Server] 启动失败!" << std::endl;
      test_done = true;
      return;
    }

    server_ready = true;
    std::cout << "[Server] 启动成功，监听 127.0.0.1:9998" << std::endl;

    while (!test_done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.Stop();
  });

  // 等待 Server 准备好
  while (!server_ready) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // 创建多个客户端
  const int num_client_threads = std::min(10, config.num_connections);
  const int connections_per_thread = config.num_connections / num_client_threads;

  std::vector<std::thread> client_threads;

  for (int t = 0; t < num_client_threads; t++) {
    client_threads.emplace_back([&, t]() {
      int base_index = t * connections_per_thread;
      int end_index = (t == num_client_threads - 1) ?
                      config.num_connections : base_index + connections_per_thread;

      for (int i = base_index; i < end_index; i++) {
        Client client;
        std::atomic<int> messages_sent(0);
        std::atomic<int> messages_received(0);
        std::atomic<bool> connected(false);

        client.SetOnConnected([&](const ConnectionInformation& info) {
          connected = true;

          // 发送多条消息
          for (int m = 0; m < config.num_messages; m++) {
            std::vector<uint8_t> data(config.message_size, 'A' + (m % 26));
            client.SendData(data.data(), data.size());
            stats.total_messages_sent++;
            stats.total_bytes_sent += data.size();
            messages_sent++;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
          }
        });

        client.SetOnMessage([&](const std::vector<uint8_t>& data) {
          messages_received++;
          if (messages_received >= config.num_messages) {
            clients_completed++;
          }
        });

        if (!client.ConnectIPv4("127.0.0.1", 9998)) {
          stats.failed_connections++;
          continue;
        }

        // 等待所有消息传输完成
        int wait_count = 0;
        while (messages_received < config.num_messages && wait_count < 500) {
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          wait_count++;
        }

        client.Disconnect();
        stats.total_connections++;
      }
    });
  }

  // 等待所有客户端线程完成
  for (auto& t : client_threads) {
    if (t.joinable()) {
      t.join();
    }
  }

  // 等待服务器处理完剩余消息
  std::this_thread::sleep_for(std::chrono::seconds(2));

  test_done = true;
  if (server_thread.joinable()) {
    server_thread.join();
  }

  stats.Stop();
  stats.Print();

  bool success = (stats.total_connections == config.num_connections);
  std::cout << "========== 持续吞吐量测试 "
            << (success ? "通过" : "失败")
            << " ==========\n" << std::endl;

  return success;
}

//
// 测试 3: 资源泄漏检测
//
bool TestResourceLeaks(const StressTestConfig& config) {
  std::cout << "\n========== 测试 3: 资源泄漏检测 ==========" << std::endl;

  std::atomic<bool> server_ready(false);
  std::atomic<bool> test_done(false);

  Server server;

  // 启动 Server
  std::thread server_thread([&]() {
    if (!server.StartIPv4("127.0.0.1", 9997)) {
      std::cerr << "[Server] 启动失败!" << std::endl;
      test_done = true;
      return;
    }

    server_ready = true;

    while (!test_done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    server.Stop();
  });

  while (!server_ready) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // 等待 Server 稳定
  std::this_thread::sleep_for(std::chrono::seconds(1));

  // 获取 Server 启动后的资源使用（作为基准）
  struct rusage usage_baseline;
  getrusage(RUSAGE_SELF, &usage_baseline);
  long baseline_maxrss = usage_baseline.ru_maxrss;

  std::cout << "[Test] Server 启动后基准内存: " << baseline_maxrss << " KB" << std::endl;

  // 创建并销毁大量连接（多轮）
  std::vector<long> memory_after_each_round;
  const int num_rounds = 3;

  for (int round = 0; round < num_rounds; round++) {
    std::cout << "[Test] 第 " << (round + 1) << " 轮连接测试..." << std::endl;

    std::vector<std::unique_ptr<Client>> clients;
    clients.reserve(config.num_connections);

    // 连接
    for (int i = 0; i < config.num_connections; i++) {
      auto client = std::make_unique<Client>();
      if (client->ConnectIPv4("127.0.0.1", 9997)) {
        clients.push_back(std::move(client));
      }
      // 控制连接速率，避免过快
      if (i % 10 == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }

    std::cout << "[Test] 已创建 " << clients.size() << " 个连接" << std::endl;

    // 断开所有连接
    clients.clear();

    std::cout << "[Test] 已断开所有连接" << std::endl;

    // 等待资源释放
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 记录每轮后的内存使用
    struct rusage usage_current;
    getrusage(RUSAGE_SELF, &usage_current);
    long current_maxrss = usage_current.ru_maxrss;
    memory_after_each_round.push_back(current_maxrss);

    std::cout << "[Test] 第 " << (round + 1) << " 轮后内存: "
              << current_maxrss << " KB (增长: "
              << (current_maxrss - baseline_maxrss) << " KB)" << std::endl;
  }

  test_done = true;
  if (server_thread.joinable()) {
    server_thread.join();
  }

  // 等待 Server 完全停止
  std::this_thread::sleep_for(std::chrono::seconds(1));

  // 获取最终资源使用
  struct rusage usage_final;
  getrusage(RUSAGE_SELF, &usage_final);
  long final_maxrss = usage_final.ru_maxrss;

  std::cout << "\n资源使用分析:" << std::endl;
  std::cout << "基准内存 (Server 启动后): " << baseline_maxrss << " KB" << std::endl;
  std::cout << "最终内存 (测试结束): " << final_maxrss << " KB" << std::endl;

  // 检查内存是否持续增长（判断泄漏）
  // 注意：getrusage 返回的是累积最大内存，不是当前内存
  // 所以 final_maxrss >= baseline_maxrss 是正常的
  // 我们主要关注每轮之间的相对增长率
  bool has_leak = false;
  long total_growth = 0;
  long max_growth = 0;

  for (size_t i = 1; i < memory_after_each_round.size(); i++) {
    long growth = memory_after_each_round[i] - memory_after_each_round[i - 1];
    total_growth += growth;
    if (growth > max_growth) {
      max_growth = growth;
    }

    // 如果某轮内存增长超过 500MB，可能存在严重泄漏
    if (growth > 500000) {
      std::cout << "[ERROR] 第 " << (i + 1) << " 轮内存激增 " << growth << " KB" << std::endl;
      has_leak = true;
    }
  }

  // 计算平均增长和增长率
  double avg_growth = total_growth / (double)num_rounds;
  std::cout << "总内存增长: " << total_growth << " KB" << std::endl;
  std::cout << "平均每轮增长: " << std::fixed << std::setprecision(0)
            << avg_growth << " KB" << std::endl;
  std::cout << "最大单轮增长: " << max_growth << " KB" << std::endl;

  // 计算增长率是否在加速（后面轮次是否比前面增长更多）
  bool is_accelerating = false;
  if (memory_after_each_round.size() >= 3) {
    long first_growth = memory_after_each_round[1] - memory_after_each_round[0];
    long last_growth = memory_after_each_round[num_rounds - 1] - memory_after_each_round[num_rounds - 2];

    // 如果最后一轮增长是第一轮的 2 倍以上，说明在加速
    if (last_growth > first_growth * 2 && first_growth > 0) {
      std::cout << "[WARNING] 内存增长在加速: 首轮 " << first_growth
                << " KB -> 末轮 " << last_growth << " KB" << std::endl;
      is_accelerating = true;
    }
  }

  // 判断标准：
  // 1. 没有单轮超过 500MB 的剧烈增长
  // 2. 内存增长没有持续加速
  bool no_leak = !has_leak && !is_accelerating;

  // 额外信息
  if (no_leak) {
    std::cout << "[INFO] 内存增长在可接受范围内" << std::endl;
    std::cout << "[INFO] 内存增长主要来自 Client 对象的 Reactor/WorkerPool" << std::endl;
  } else {
    std::cout << "[WARNING] 可能存在内存泄漏或资源未正确释放" << std::endl;
    std::cout << "[WARNING] 建议使用 leaks 工具进行详细检查" << std::endl;
  }

  std::cout << "\n========== 资源泄漏检测 "
            << (no_leak ? "通过" : "发现可疑泄漏")
            << " ==========\n" << std::endl;

  return no_leak;
}

//
// 主函数
//
int main(int argc, char* argv[]) {
  std::cout << "========================================" << std::endl;
  std::cout << "  DarwinCore - 高并发压力测试" << std::endl;
  std::cout << "========================================" << std::endl;

  // 忽略 SIGPIPE
  signal(SIGPIPE, SIG_IGN);

  // 测试配置
  StressTestConfig config;

  // 解析命令行参数
  if (argc > 1) {
    std::string test_type = argv[1];

    if (test_type == "small") {
      config.num_connections = 100;
      config.num_messages = 10;
      config.message_size = 1024;
      config.num_reactor_threads = 2;
      config.num_worker_threads = 2;
    } else if (test_type == "medium") {
      config.num_connections = 500;
      config.num_messages = 100;
      config.message_size = 4096;
      config.num_reactor_threads = 4;
      config.num_worker_threads = 4;
    } else if (test_type == "large") {
      config.num_connections = 1000;
      config.num_messages = 1000;
      config.message_size = 8192;
      config.num_reactor_threads = 8;
      config.num_worker_threads = 8;
    } else if (test_type == "extreme") {
      config.num_connections = 1000;
      config.num_messages = 5000;
      config.message_size = 1024;
      config.num_reactor_threads = 16;
      config.num_worker_threads = 16;
    } else {
      std::cerr << "用法: " << argv[0] << " [small|medium|large|extreme]" << std::endl;
      return 1;
    }
  } else {
    // 默认中等规模
    config.num_connections = 500;
    config.num_messages = 100;
    config.message_size = 4096;
    config.num_reactor_threads = 4;
    config.num_worker_threads = 4;
  }

  config.verbose = false;

  std::cout << "\n测试配置:" << std::endl;
  std::cout << "  并发连接: " << config.num_connections << std::endl;
  std::cout << "  每连接消息: " << config.num_messages << std::endl;
  std::cout << "  消息大小: " << config.message_size << " 字节" << std::endl;
  std::cout << "  Reactor 线程: " << config.num_reactor_threads << std::endl;
  std::cout << "  Worker 线程: " << config.num_worker_threads << std::endl;

  // 估算线程数（警告）
  int estimated_threads = config.num_connections * 2 + config.num_reactor_threads + config.num_worker_threads;
  std::cout << "\n[INFO] 预估线程数: ~" << estimated_threads << std::endl;
  std::cout << "[INFO] 系统限制: " << sysconf(_SC_CHILD_MAX) << " (ulimit -u)" << std::endl;

  if (estimated_threads > 2000) {
    std::cout << "\n[WARNING] 预估线程数接近系统限制！" << std::endl;
    std::cout << "[WARNING] 建议运行 'ulimit -u 4096' 增加限制" << std::endl;
  }

  // 运行测试
  bool pass1 = TestAcceptStorm(config);
  bool pass2 = TestSustainedThroughput(config);
  bool pass3 = TestResourceLeaks(config);

  std::cout << "\n========================================" << std::endl;
  std::cout << "  测试总结" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "Accept 风暴:    " << (pass1 ? "✓ 通过" : "✗ 失败") << std::endl;
  std::cout << "持续吞吐量:     " << (pass2 ? "✓ 通过" : "✗ 失败") << std::endl;
  std::cout << "资源泄漏:       " << (pass3 ? "✓ 通过" : "✗ 失败") << std::endl;
  std::cout << "========================================" << std::endl;

  return (pass1 && pass2 && pass3) ? 0 : 1;
}
