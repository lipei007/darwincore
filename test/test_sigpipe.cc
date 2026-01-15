//
// 测试 SIGPIPE 处理
//
// 验证 Server 和 Client 是否正确忽略 SIGPIPE
//

#include <iostream>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>

#include <darwincore/network/server.h>
#include <darwincore/network/client.h>

using namespace darwincore::network;

void TestSIGPIPEHandling() {
  std::cout << "========== SIGPIPE 处理测试 ==========" << std::endl;

  // 检查 SIGPIPE 是否被忽略
  struct sigaction sa;
  sigaction(SIGPIPE, nullptr, &sa);

  if (sa.sa_handler == SIG_IGN) {
    std::cout << "✓ SIGPIPE 已被忽略" << std::endl;
  } else {
    std::cout << "✗ SIGPIPE 未被忽略 (handler=" << (void*)sa.sa_handler << ")" << std::endl;
  }

  std::cout << "\n========== 测试完成 ==========" << std::endl;
}

int main() {
  TestSIGPIPEHandling();

  // 创建一个 Server 和 Client，验证构造函数会自动设置 SIGPIPE
  std::cout << "\n创建 Server..." << std::endl;
  Server server;

  std::cout << "创建 Client..." << std::endl;
  Client client;

  std::cout << "\n再次检查 SIGPIPE..." << std::endl;
  TestSIGPIPEHandling();

  return 0;
}
