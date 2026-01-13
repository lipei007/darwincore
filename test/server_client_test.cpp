//
// DarwinCore Network 模块综合测试
//
// 测试场景：
//   1. IPv4 TCP 通信测试
//   2. IPv6 TCP 通信测试
//   3. Unix Domain Socket 通信测试
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cstring>
#include <string>
#include <unistd.h>
#include <darwincore/network/server.h>
#include <darwincore/network/client.h>

using namespace darwincore::network;

// 测试场景枚举
enum class TestScenario {
  kIPv4,
  kIPv6,
  kUnixDomain
};

// IPv4 测试
bool TestIPv4() {
  std::cout << "\n========== IPv4 测试开始 ==========" << std::endl;

  std::atomic<bool> server_ready(false);
  std::atomic<bool> client_connected(false);
  std::atomic<bool> server_received_message(false);
  std::atomic<bool> client_received_reply(false);
  std::atomic<bool> test_done(false);

  Server* g_server = nullptr;
  Client* g_client = nullptr;

  // 启动 Server 线程
  std::thread server_thread([&]() {
    Server server;
    g_server = &server;

    server.SetOnClientConnected([&](const ConnectionInformation& info) {
      std::cout << "[Server-IPv4] 客户端已连接: " << info.peer_address
                << ":" << info.peer_port << std::endl;
      client_connected = true;
    });

    server.SetOnMessage([&](uint64_t conn_id, const std::vector<uint8_t>& data) {
      std::string msg(data.begin(), data.end());
      std::cout << "[Server-IPv4] 收到消息: " << msg << std::endl;

      // 回复消息
      if (g_server) {
        std::string reply = "IPv4 Server 回复: " + msg;
        g_server->SendData(conn_id, reinterpret_cast<const uint8_t*>(reply.c_str()),
                          reply.length());
      }
      server_received_message = true;
    });

    server.SetOnClientDisconnected([](uint64_t conn_id) {
      std::cout << "[Server-IPv4] 客户端断开连接, conn_id=" << conn_id << std::endl;
    });

    server.SetOnConnectionError([](uint64_t conn_id, NetworkError error,
                                  const std::string& message) {
      std::cout << "[Server-IPv4] 连接错误: " << static_cast<int>(error)
                << ", " << message << std::endl;
    });

    if (!server.StartIPv4("127.0.0.1", 9999)) {
      std::cerr << "[Server-IPv4] 启动失败!" << std::endl;
      test_done = true;
      return;
    }

    server_ready = true;
    std::cout << "[Server-IPv4] 启动成功，等待连接..." << std::endl;

    // 等待测试完成
    while (!test_done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[Server-IPv4] 测试完成，停止中..." << std::endl;
    server.Stop();
    g_server = nullptr;
    std::cout << "[Server-IPv4] 已停止" << std::endl;
  });

  // 启动 Client 线程
  std::thread client_thread([&]() {
    // 等待 Server 准备好
    while (!server_ready) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "[Client-IPv4] 连接中..." << std::endl;

    Client client;
    g_client = &client;

    client.SetOnConnected([&](const ConnectionInformation& info) {
      std::cout << "[Client-IPv4] 已连接到服务器" << std::endl;
    });

    client.SetOnMessage([&](const std::vector<uint8_t>& data) {
      std::string msg(data.begin(), data.end());
      std::cout << "[Client-IPv4] 收到消息: " << msg << std::endl;
      client_received_reply = true;
    });

    client.SetOnDisconnected([]() {
      std::cout << "[Client-IPv4] 已断开连接" << std::endl;
    });

    client.SetOnError([](NetworkError error, const std::string& message) {
      std::cout << "[Client-IPv4] 错误: " << static_cast<int>(error)
                << ", " << message << std::endl;
    });

    if (!client.ConnectIPv4("127.0.0.1", 9999)) {
      std::cerr << "[Client-IPv4] 连接失败!" << std::endl;
      test_done = true;
      return;
    }

    // 等待连接建立
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "[Client-IPv4] 发送测试消息..." << std::endl;
    std::string msg = "Hello IPv4!";
    client.SendData(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length());

    // 等待接收回复
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::cout << "[Client-IPv4] 测试完成，断开连接..." << std::endl;
    client.Disconnect();
    g_client = nullptr;
    test_done = true;
  });

  // 等待测试完成
  while (!test_done) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // 等待线程结束
  if (server_thread.joinable()) {
    server_thread.join();
  }
  if (client_thread.joinable()) {
    client_thread.join();
  }

  // 验证结果
  bool success = client_connected && server_received_message && client_received_reply;
  std::cout << "========== IPv4 测试 "
            << (success ? "通过" : "失败")
            << " ==========\n" << std::endl;

  return success;
}

// IPv6 测试
bool TestIPv6() {
  std::cout << "\n========== IPv6 测试开始 ==========" << std::endl;

  std::atomic<bool> server_ready(false);
  std::atomic<bool> client_connected(false);
  std::atomic<bool> server_received_message(false);
  std::atomic<bool> client_received_reply(false);
  std::atomic<bool> test_done(false);

  Server* g_server = nullptr;
  Client* g_client = nullptr;

  // 启动 Server 线程
  std::thread server_thread([&]() {
    Server server;
    g_server = &server;

    server.SetOnClientConnected([&](const ConnectionInformation& info) {
      std::cout << "[Server-IPv6] 客户端已连接: " << info.peer_address
                << ":" << info.peer_port << std::endl;
      client_connected = true;
    });

    server.SetOnMessage([&](uint64_t conn_id, const std::vector<uint8_t>& data) {
      std::string msg(data.begin(), data.end());
      std::cout << "[Server-IPv6] 收到消息: " << msg << std::endl;

      // 回复消息
      if (g_server) {
        std::string reply = "IPv6 Server 回复: " + msg;
        g_server->SendData(conn_id, reinterpret_cast<const uint8_t*>(reply.c_str()),
                          reply.length());
      }
      server_received_message = true;
    });

    server.SetOnClientDisconnected([](uint64_t conn_id) {
      std::cout << "[Server-IPv6] 客户端断开连接, conn_id=" << conn_id << std::endl;
    });

    server.SetOnConnectionError([](uint64_t conn_id, NetworkError error,
                                  const std::string& message) {
      std::cout << "[Server-IPv6] 连接错误: " << static_cast<int>(error)
                << ", " << message << std::endl;
    });

    if (!server.StartIPv6("::1", 9998)) {
      std::cerr << "[Server-IPv6] 启动失败!" << std::endl;
      test_done = true;
      return;
    }

    server_ready = true;
    std::cout << "[Server-IPv6] 启动成功，等待连接..." << std::endl;

    // 等待测试完成
    while (!test_done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[Server-IPv6] 测试完成，停止中..." << std::endl;
    server.Stop();
    g_server = nullptr;
    std::cout << "[Server-IPv6] 已停止" << std::endl;
  });

  // 启动 Client 线程
  std::thread client_thread([&]() {
    // 等待 Server 准备好
    while (!server_ready) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "[Client-IPv6] 连接中..." << std::endl;

    Client client;
    g_client = &client;

    client.SetOnConnected([&](const ConnectionInformation& info) {
      std::cout << "[Client-IPv6] 已连接到服务器" << std::endl;
    });

    client.SetOnMessage([&](const std::vector<uint8_t>& data) {
      std::string msg(data.begin(), data.end());
      std::cout << "[Client-IPv6] 收到消息: " << msg << std::endl;
      client_received_reply = true;
    });

    client.SetOnDisconnected([]() {
      std::cout << "[Client-IPv6] 已断开连接" << std::endl;
    });

    client.SetOnError([](NetworkError error, const std::string& message) {
      std::cout << "[Client-IPv6] 错误: " << static_cast<int>(error)
                << ", " << message << std::endl;
    });

    if (!client.ConnectIPv6("::1", 9998)) {
      std::cerr << "[Client-IPv6] 连接失败!" << std::endl;
      test_done = true;
      return;
    }

    // 等待连接建立
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "[Client-IPv6] 发送测试消息..." << std::endl;
    std::string msg = "Hello IPv6!";
    client.SendData(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length());

    // 等待接收回复
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::cout << "[Client-IPv6] 测试完成，断开连接..." << std::endl;
    client.Disconnect();
    g_client = nullptr;
    test_done = true;
  });

  // 等待测试完成
  while (!test_done) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // 等待线程结束
  if (server_thread.joinable()) {
    server_thread.join();
  }
  if (client_thread.joinable()) {
    client_thread.join();
  }

  // 验证结果
  bool success = client_connected && server_received_message && client_received_reply;
  std::cout << "========== IPv6 测试 "
            << (success ? "通过" : "失败")
            << " ==========\n" << std::endl;

  return success;
}

// Unix Domain Socket 测试
bool TestUnixDomain() {
  std::cout << "\n========== Unix Domain Socket 测试开始 ==========" << std::endl;

  std::string socket_path = "/tmp/dc_test.sock";

  std::atomic<bool> server_ready(false);
  std::atomic<bool> client_connected(false);
  std::atomic<bool> server_received_message(false);
  std::atomic<bool> client_received_reply(false);
  std::atomic<bool> test_done(false);

  Server* g_server = nullptr;
  Client* g_client = nullptr;

  // 启动 Server 线程
  std::thread server_thread([&]() {
    Server server;
    g_server = &server;

    server.SetOnClientConnected([&](const ConnectionInformation& info) {
      std::cout << "[Server-UDS] 客户端已连接: " << info.peer_address << std::endl;
      client_connected = true;
    });

    server.SetOnMessage([&](uint64_t conn_id, const std::vector<uint8_t>& data) {
      std::string msg(data.begin(), data.end());
      std::cout << "[Server-UDS] 收到消息: " << msg << std::endl;

      // 回复消息
      if (g_server) {
        std::string reply = "UDS Server 回复: " + msg;
        g_server->SendData(conn_id, reinterpret_cast<const uint8_t*>(reply.c_str()),
                          reply.length());
      }
      server_received_message = true;
    });

    server.SetOnClientDisconnected([](uint64_t conn_id) {
      std::cout << "[Server-UDS] 客户端断开连接, conn_id=" << conn_id << std::endl;
    });

    server.SetOnConnectionError([](uint64_t conn_id, NetworkError error,
                                  const std::string& message) {
      std::cout << "[Server-UDS] 连接错误: " << static_cast<int>(error)
                << ", " << message << std::endl;
    });

    if (!server.StartUnixDomain(socket_path)) {
      std::cerr << "[Server-UDS] 启动失败!" << std::endl;
      test_done = true;
      return;
    }

    server_ready = true;
    std::cout << "[Server-UDS] 启动成功，等待连接..." << std::endl;

    // 等待测试完成
    while (!test_done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "[Server-UDS] 测试完成，停止中..." << std::endl;
    server.Stop();
    g_server = nullptr;
    std::cout << "[Server-UDS] 已停止" << std::endl;
  });

  // 启动 Client 线程
  std::thread client_thread([&]() {
    // 等待 Server 准备好
    while (!server_ready) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "[Client-UDS] 连接中..." << std::endl;

    Client client;
    g_client = &client;

    client.SetOnConnected([&](const ConnectionInformation& info) {
      std::cout << "[Client-UDS] 已连接到服务器" << std::endl;
    });

    client.SetOnMessage([&](const std::vector<uint8_t>& data) {
      std::string msg(data.begin(), data.end());
      std::cout << "[Client-UDS] 收到消息: " << msg << std::endl;
      client_received_reply = true;
    });

    client.SetOnDisconnected([]() {
      std::cout << "[Client-UDS] 已断开连接" << std::endl;
    });

    client.SetOnError([&](NetworkError error, const std::string& message) {
      std::cout << "[Client-UDS] 错误: " << static_cast<int>(error)
                << ", " << message << std::endl;
    });

    // 等待 socket 文件创建
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (!client.ConnectUnixDomain(socket_path)) {
      std::cerr << "[Client-UDS] 连接失败!" << std::endl;
      test_done = true;
      return;
    }

    // 等待连接建立
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::cout << "[Client-UDS] 发送测试消息..." << std::endl;
    std::string msg = "Hello Unix Domain Socket!";
    client.SendData(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length());

    // 等待接收回复
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    std::cout << "[Client-UDS] 测试完成，断开连接..." << std::endl;
    client.Disconnect();
    g_client = nullptr;
    test_done = true;
  });

  // 等待测试完成
  while (!test_done) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  // 等待线程结束
  if (server_thread.joinable()) {
    server_thread.join();
  }
  if (client_thread.joinable()) {
    client_thread.join();
  }

  // 验证结果
  bool success = client_connected && server_received_message && client_received_reply;
  std::cout << "========== Unix Domain Socket 测试 "
            << (success ? "通过" : "失败")
            << " ==========\n" << std::endl;

  return success;
}

int main(int argc, char* argv[]) {
  std::cout << "========================================" << std::endl;
  std::cout << "  DarwinCore Network 模块综合测试" << std::endl;
  std::cout << "========================================" << std::endl;

  // 解析命令行参数
  TestScenario scenario = TestScenario::kIPv4;
  if (argc > 1) {
    std::string arg = argv[1];
    if (arg == "ipv4") {
      scenario = TestScenario::kIPv4;
    } else if (arg == "ipv6") {
      scenario = TestScenario::kIPv6;
    } else if (arg == "uds" || arg == "unix") {
      scenario = TestScenario::kUnixDomain;
    } else if (arg == "all") {
      // 运行所有测试
      bool ipv4_pass = TestIPv4();
      bool ipv6_pass = TestIPv6();
      bool uds_pass = TestUnixDomain();

      std::cout << "\n========================================" << std::endl;
      std::cout << "  测试总结" << std::endl;
      std::cout << "========================================" << std::endl;
      std::cout << "IPv4 测试:      " << (ipv4_pass ? "✓ 通过" : "✗ 失败") << std::endl;
      std::cout << "IPv6 测试:      " << (ipv6_pass ? "✓ 通过" : "✗ 失败") << std::endl;
      std::cout << "UDS 测试:       " << (uds_pass ? "✓ 通过" : "✗ 失败") << std::endl;
      std::cout << "========================================" << std::endl;

      return (ipv4_pass && ipv6_pass && uds_pass) ? 0 : 1;
    } else {
      std::cerr << "未知参数: " << arg << std::endl;
      std::cerr << "用法: " << argv[0] << " [ipv4|ipv6|uds|all]" << std::endl;
      return 1;
    }
  }

  // 运行单个测试
  bool pass = false;
  switch (scenario) {
    case TestScenario::kIPv4:
      pass = TestIPv4();
      break;
    case TestScenario::kIPv6:
      pass = TestIPv6();
      break;
    case TestScenario::kUnixDomain:
      pass = TestUnixDomain();
      break;
  }

  return pass ? 0 : 1;
}
