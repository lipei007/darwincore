#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "reactor.h"
#include "worker_pool.h"

using namespace darwincore::network;

namespace {

bool WaitUntil(const std::function<bool()> &pred,
               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return pred();
}

bool SetNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool Check(bool cond, const char *msg) {
  if (!cond) {
    std::cerr << "[FAIL] " << msg << std::endl;
    return false;
  }
  std::cout << "[PASS] " << msg << std::endl;
  return true;
}

} // namespace

int main() {
  auto worker_pool = std::make_shared<WorkerPool>(1, 1024);
  std::atomic<uint64_t> conn_id{0};

  worker_pool->SetEventCallback([&](const NetworkEvent &event) {
    if (event.type == NetworkEventType::kConnected) {
      conn_id.store(event.connection_id, std::memory_order_relaxed);
    }
  });

  bool ok = true;
  ok &= Check(worker_pool->Start(), "worker starts");

  Reactor reactor(0, worker_pool);
  ok &= Check(reactor.Start(), "reactor starts");

  int fds[2] = {-1, -1};
  ok &= Check(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0,
              "socketpair created");
  ok &= Check(SetNonBlocking(fds[0]), "socketpair[0] non-blocking");
  ok &= Check(SetNonBlocking(fds[1]), "socketpair[1] non-blocking");

  sockaddr_storage peer{};
  auto *sun = reinterpret_cast<sockaddr_un *>(&peer);
  sun->sun_family = AF_UNIX;
  std::strncpy(sun->sun_path, "reactor-op-stats", sizeof(sun->sun_path) - 1);

  ok &= Check(reactor.AddConnection(fds[0], peer), "add connection request accepted");
  ok &= Check(WaitUntil([&] { return conn_id.load(std::memory_order_relaxed) != 0; },
                        std::chrono::milliseconds(500)),
              "connected event observed");

  const auto id = conn_id.load(std::memory_order_relaxed);
  const uint8_t payload[] = {1, 2, 3, 4, 5};
  ok &= Check(reactor.SendData(id, payload, sizeof(payload)),
              "valid send request accepted");
  ok &= Check(reactor.SendData(0xFFFFFFFFFFFFFFFFULL, payload, sizeof(payload)),
              "invalid send request still enqueued");

  ok &= Check(reactor.RemoveConnection(0xFFFFFFFFFFFFFFFFULL),
              "invalid remove request enqueued");
  ok &= Check(reactor.RemoveConnection(id), "valid remove request enqueued");

  ok &= Check(WaitUntil([&] {
                  const auto stats = reactor.GetStatistics();
                  return stats.total_send_failures >= 1 &&
                         stats.total_remove_failures >= 1 &&
                         stats.total_add_requests >= 1;
                }, std::chrono::seconds(1)),
              "operation failure counters updated");

  const auto before_stop_stats = reactor.GetStatistics();
  ok &= Check(before_stop_stats.total_send_requests >= 2,
              "send request counter includes valid+invalid");
  ok &= Check(before_stop_stats.total_remove_requests >= 2,
              "remove request counter includes valid+invalid");
  ok &= Check(reactor.GetSendBufferSize(id) == 0,
              "thread-safe send buffer query works after remove");

  reactor.Stop();
  worker_pool->Stop();
  if (fds[1] >= 0) {
    close(fds[1]);
  }

  return ok ? 0 : 1;
}
