//
// DarwinCore Network 模块 - kqueue 核心机制测试
//
// 测试目标：
//   1. kqueue 事件注册与触发的正确性
//   2. EV_EOF / EV_ERROR 处理
//   3. EVFILT_READ / WRITE 语义验证
//   4. udata 生命周期安全
//   5. 非阻塞 IO 的所有返回值路径
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <cassert>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/event.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// 测试统计
struct TestStats {
  std::atomic<int> total_tests{0};
  std::atomic<int> passed_tests{0};
  std::atomic<int> failed_tests{0};

  void RecordPass() {
    total_tests++;
    passed_tests++;
  }

  void RecordFail() {
    total_tests++;
    failed_tests++;
  }

  void Print() {
    std::cout << "\n========== 测试统计 ==========" << std::endl;
    std::cout << "总测试数: " << total_tests << std::endl;
    std::cout << "通过: " << passed_tests << " ✓" << std::endl;
    std::cout << "失败: " << failed_tests << " ✗" << std::endl;
    std::cout << "==============================\n" << std::endl;
  }
};

static TestStats g_stats;

// 辅助宏
#define TEST_ASSERT(condition, message) \
  do { \
    if (condition) { \
      std::cout << "[PASS] " << message << std::endl; \
      g_stats.RecordPass(); \
    } else { \
      std::cerr << "[FAIL] " << message << " (line " << __LINE__ << ")" << std::endl; \
      g_stats.RecordFail(); \
    } \
  } while(0)

//
// 测试 1: kqueue 基本事件注册与触发
//
bool TestKqueueBasicEventTriggering() {
  std::cout << "\n========== 测试 1: kqueue 基本事件注册与触发 ==========" << std::endl;

  int kq = kqueue();
  TEST_ASSERT(kq >= 0, "创建 kqueue");
  if (kq < 0) return false;

  // 创建管道用于测试
  int pipefd[2];
  int ret = pipe(pipefd);
  TEST_ASSERT(ret == 0, "创建管道");
  if (ret != 0) {
    close(kq);
    return false;
  }

  // 设置非阻塞
  fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

  // 注册读事件
  struct kevent change;
  EV_SET(&change, pipefd[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  ret = kevent(kq, &change, 1, nullptr, 0, nullptr);
  TEST_ASSERT(ret == 0, "注册 EVFILT_READ 事件");

  // 测试 1.1: 无数据时不应触发事件
  struct kevent event;
  struct timespec timeout1 = {0, 100000000}; // 100ms
  ret = kevent(kq, nullptr, 0, &event, 1, &timeout1);
  TEST_ASSERT(ret == 0, "无数据时 kevent 超时返回（未触发事件）");

  // 测试 1.2: 写入数据后应触发读事件
  const char* test_msg = "Hello kqueue";
  ssize_t written = write(pipefd[1], test_msg, strlen(test_msg));
  TEST_ASSERT(written == strlen(test_msg), "写入数据到管道");

  ret = kevent(kq, nullptr, 0, &event, 1, nullptr);
  TEST_ASSERT(ret == 1, "有数据时 kevent 返回 1 个事件");
  TEST_ASSERT(event.filter == EVFILT_READ, "事件类型为 EVFILT_READ");
  TEST_ASSERT(event.ident == (unsigned)pipefd[0], "事件 ident 为正确的 fd");

  // 测试 1.3: 读取数据后，事件应该再次可触发（level-triggered）
  char buffer[128];
  ssize_t nread = read(pipefd[0], buffer, sizeof(buffer));
  TEST_ASSERT(nread == strlen(test_msg), "读取数据成功");

  // 再次写入
  write(pipefd[1], test_msg, strlen(test_msg));

  ret = kevent(kq, nullptr, 0, &event, 1, nullptr);
  TEST_ASSERT(ret == 1, "level-triggered: 数据仍存在时再次触发");

  // 清理
  close(pipefd[0]);
  close(pipefd[1]);
  close(kq);

  return true;
}

//
// 测试 2: EV_EOF 行为验证
//
bool TestEVEOFBehavior() {
  std::cout << "\n========== 测试 2: EV_EOF 行为验证 ==========" << std::endl;

  int kq = kqueue();
  if (kq < 0) return false;

  // 创建 socket 对
  int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  int reuse = 1;
  setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0); // 随机端口

  bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
  listen(listen_fd, 1);

  // 获取监听端口
  socklen_t len = sizeof(addr);
  getsockname(listen_fd, (struct sockaddr*)&addr, &len);
  uint16_t port = ntohs(addr.sin_port);

  // 连接
  int client_fd = socket(AF_INET, SOCK_STREAM, 0);
  connect(client_fd, (struct sockaddr*)&addr, sizeof(addr));

  int server_fd = accept(listen_fd, nullptr, nullptr);

  // 设置非阻塞
  fcntl(server_fd, F_SETFL, O_NONBLOCK);

  // 注册读事件
  struct kevent change;
  EV_SET(&change, server_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  kevent(kq, &change, 1, nullptr, 0, nullptr);

  // 测试 2.1: 对端 close() 应该触发 EV_EOF
  close(client_fd);

  struct kevent event;
  int ret = kevent(kq, nullptr, 0, &event, 1, nullptr);
  TEST_ASSERT(ret == 1, "对端 close 后触发事件");
  TEST_ASSERT(event.flags & EV_EOF, "事件包含 EV_EOF 标志");

  // 测试 2.2: EV_EOF 时 recv 应该返回 0
  char buffer[128];
  ssize_t nread = recv(server_fd, buffer, sizeof(buffer), 0);
  TEST_ASSERT(nread == 0, "EV_EOF 时 recv 返回 0");

  // 清理
  close(server_fd);
  close(listen_fd);
  close(kq);

  return true;
}

//
// 测试 3: udata 生命周期安全
//
bool TestUdataLifecycle() {
  std::cout << "\n========== 测试 3: udata 生命周期安全 ==========" << std::endl;

  int kq = kqueue();
  if (kq < 0) return false;

  int pipefd[2];
  pipe(pipefd);

  // 创建一个动态分配的 udata
  struct Udata {
    int magic;
    int value;
  };
  Udata* udata = new Udata{static_cast<int>(0xDEADBEEF), 42};

  // 注册事件时设置 udata
  struct kevent change;
  EV_SET(&change, pipefd[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, udata);
  kevent(kq, &change, 1, nullptr, 0, nullptr);

  // 写入数据
  write(pipefd[1], "test", 4);

  // 获取事件
  struct kevent event;
  int ret = kevent(kq, nullptr, 0, &event, 1, nullptr);
  TEST_ASSERT(ret == 1, "获取到事件");

  // 验证 udata 正确返回
  Udata* returned_udata = static_cast<Udata*>(event.udata);
  TEST_ASSERT(returned_udata != nullptr, "udata 不为空");
  TEST_ASSERT(returned_udata->magic == 0xDEADBEEF, "udata magic 值正确");
  TEST_ASSERT(returned_udata->value == 42, "udata value 值正确");

  // 清理
  delete udata;
  close(pipefd[0]);
  close(pipefd[1]);
  close(kq);

  return true;
}

//
// 测试 4: EV_ADD / EV_DELETE / EV_ENABLE / EV_DISABLE
//
bool TestEventFlags() {
  std::cout << "\n========== 测试 4: EV_ADD / EV_DELETE / EV_ENABLE / EV_DISABLE ==========" << std::endl;

  int kq = kqueue();
  if (kq < 0) return false;

  int pipefd[2];
  pipe(pipefd);

  // 测试 4.1: EV_ADD
  struct kevent change;
  EV_SET(&change, pipefd[0], EVFILT_READ, EV_ADD, 0, 0, nullptr);
  int ret = kevent(kq, &change, 1, nullptr, 0, nullptr);
  TEST_ASSERT(ret == 0, "EV_ADD 成功");

  // 测试 4.2: EV_DISABLE
  EV_SET(&change, pipefd[0], EVFILT_READ, EV_DISABLE, 0, 0, nullptr);
  ret = kevent(kq, &change, 1, nullptr, 0, nullptr);
  TEST_ASSERT(ret == 0, "EV_DISABLE 成功");

  // 写入数据
  write(pipefd[1], "test", 4);

  // 验证被禁用后不会触发事件
  struct kevent event;
  struct timespec timeout = {0, 100000000}; // 100ms
  ret = kevent(kq, nullptr, 0, &event, 1, &timeout);
  TEST_ASSERT(ret == 0, "EV_DISABLE 后未触发事件");

  // 测试 4.3: EV_ENABLE
  EV_SET(&change, pipefd[0], EVFILT_READ, EV_ENABLE, 0, 0, nullptr);
  ret = kevent(kq, &change, 1, nullptr, 0, nullptr);
  TEST_ASSERT(ret == 0, "EV_ENABLE 成功");

  // 现在应该能触发事件
  ret = kevent(kq, nullptr, 0, &event, 1, nullptr);
  TEST_ASSERT(ret == 1, "EV_ENABLE 后触发事件");

  // 测试 4.4: EV_DELETE
  EV_SET(&change, pipefd[0], EVFILT_READ, EV_DELETE, 0, 0, nullptr);
  ret = kevent(kq, &change, 1, nullptr, 0, nullptr);
  TEST_ASSERT(ret == 0, "EV_DELETE 成功");

  // 清理
  close(pipefd[0]);
  close(pipefd[1]);
  close(kq);

  return true;
}

//
// 测试 5: 非阻塞 IO 返回值全覆盖测试
//
bool TestNonblockingIOReturnValues() {
  std::cout << "\n========== 测试 5: 非阻塞 IO 返回值测试 ==========" << std::endl;

  // 创建一对 socket
  int fds[2];
  int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
  TEST_ASSERT(ret == 0, "创建 socket pair");
  if (ret != 0) return false;

  // 设置非阻塞
  fcntl(fds[0], F_SETFL, O_NONBLOCK);
  fcntl(fds[1], F_SETFL, O_NONBLOCK);

  // 测试 5.1: recv 返回 EAGAIN（无数据可读）
  char buffer[128];
  ret = recv(fds[0], buffer, sizeof(buffer), 0);
  TEST_ASSERT(ret < 0 && errno == EAGAIN, "无数据时 recv 返回 EAGAIN");

  // 测试 5.2: send/recv 成功（>0）
  const char* msg = "Hello";
  ssize_t sent = send(fds[1], msg, strlen(msg), 0);
  TEST_ASSERT(sent > 0, "send 成功返回 > 0");

  ssize_t nread = recv(fds[0], buffer, sizeof(buffer), 0);
  TEST_ASSERT(nread > 0, "recv 成功返回 > 0");
  TEST_ASSERT(nread == strlen(msg), "recv 读取正确的字节数");

  // 测试 5.3: 对端关闭后 recv 返回 0
  close(fds[1]);
  nread = recv(fds[0], buffer, sizeof(buffer), 0);
  TEST_ASSERT(nread == 0, "对端关闭后 recv 返回 0");

  // 测试 5.4: 向已关闭的 socket 写入返回 EPIPE
  ret = send(fds[0], msg, strlen(msg), 0);
  TEST_ASSERT(ret < 0 && (errno == EPIPE || errno == ECONNRESET), "写已关闭 socket 返回 EPIPE/ECONNRESET");

  close(fds[0]);

  return true;
}

//
// 测试 6: EVFILT_WRITE 语义（特别注意 macOS 行为）
//
bool TestWriteFilterSemantics() {
  std::cout << "\n========== 测试 6: EVFILT_WRITE 语义 ==========" << std::endl;

  int kq = kqueue();
  if (kq < 0) return false;

  int fds[2];
  socketpair(AF_UNIX, SOCK_STREAM, 0, fds);

  // 注册写事件
  struct kevent change;
  EV_SET(&change, fds[0], EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  int ret = kevent(kq, &change, 1, nullptr, 0, nullptr);
  TEST_ASSERT(ret == 0, "注册 EVFILT_WRITE");

  // 测试 6.1: 在 macOS 上，写事件几乎总是就绪
  struct kevent event;
  struct timespec timeout_write = {0, 10000000}; // 10ms
  ret = kevent(kq, nullptr, 0, &event, 1, &timeout_write);
  TEST_ASSERT(ret == 1, "EVFILT_WRITE 立即就绪（macOS 行为）");

  // 测试 6.2: 不需要 WRITE 时应该 EV_DISABLE，避免 busy-loop
  EV_SET(&change, fds[0], EVFILT_WRITE, EV_DISABLE, 0, 0, nullptr);
  ret = kevent(kq, &change, 1, nullptr, 0, nullptr);
  TEST_ASSERT(ret == 0, "禁用 EVFILT_WRITE");

  // 清理
  close(fds[0]);
  close(fds[1]);
  close(kq);

  return true;
}

//
// 测试 7: 批量事件处理
//
bool TestBatchEventProcessing() {
  std::cout << "\n========== 测试 7: 批量事件处理 ==========" << std::endl;

  int kq = kqueue();
  if (kq < 0) return false;

  const int NUM_PIPES = 10;
  int pipes[NUM_PIPES][2];

  // 创建多个管道
  for (int i = 0; i < NUM_PIPES; i++) {
    pipe(pipes[i]);
  }

  // 注册所有读事件
  struct kevent changes[NUM_PIPES];
  for (int i = 0; i < NUM_PIPES; i++) {
    EV_SET(&changes[i], pipes[i][0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  }
  int ret = kevent(kq, changes, NUM_PIPES, nullptr, 0, nullptr);
  TEST_ASSERT(ret == 0, "批量注册 " + std::to_string(NUM_PIPES) + " 个事件");

  // 向所有管道写入数据
  for (int i = 0; i < NUM_PIPES; i++) {
    write(pipes[i][1], "test", 4);
  }

  // 批量获取事件
  struct kevent events[NUM_PIPES];
  ret = kevent(kq, nullptr, 0, events, NUM_PIPES, nullptr);
  TEST_ASSERT(ret == NUM_PIPES, "批量获取 " + std::to_string(NUM_PIPES) + " 个事件");

  // 清理
  for (int i = 0; i < NUM_PIPES; i++) {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }
  close(kq);

  return true;
}

//
// 主函数
//
int main(int argc, char* argv[]) {
  std::cout << "========================================" << std::endl;
  std::cout << "  DarwinCore - kqueue 核心机制测试" << std::endl;
  std::cout << "========================================" << std::endl;

  // 忽略 SIGPIPE
  signal(SIGPIPE, SIG_IGN);

  // 运行所有测试
  TestKqueueBasicEventTriggering();
  TestEVEOFBehavior();
  TestUdataLifecycle();
  TestEventFlags();
  TestNonblockingIOReturnValues();
  TestWriteFilterSemantics();
  TestBatchEventProcessing();

  // 打印统计
  g_stats.Print();

  return (g_stats.failed_tests == 0) ? 0 : 1;
}
