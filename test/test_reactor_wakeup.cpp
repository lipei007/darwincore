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
  std::atomic<bool> connected{false};
  std::atomic<bool> got_data{false};
  std::atomic<int64_t> connect_latency_ms{-1};

  worker_pool->SetEventCallback([&](const NetworkEvent &event) {
    if (event.type == NetworkEventType::kConnected) {
      connected.store(true, std::memory_order_relaxed);
    } else if (event.type == NetworkEventType::kData &&
               !event.payload.empty()) {
      got_data.store(true, std::memory_order_relaxed);
    }
  });

  bool ok = true;
  ok &= Check(worker_pool->Start(), "worker pool starts");

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
  std::strncpy(sun->sun_path, "reactor-wakeup-test", sizeof(sun->sun_path) - 1);

  auto start = std::chrono::steady_clock::now();
  ok &= Check(reactor.AddConnection(fds[0], peer), "AddConnection enqueues");
  ok &= Check(WaitUntil([&] { return connected.load(std::memory_order_relaxed); },
                        std::chrono::milliseconds(300)),
              "connected event arrives quickly");

  connect_latency_ms.store(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count(),
      std::memory_order_relaxed);

  ok &= Check(connect_latency_ms.load(std::memory_order_relaxed) < 120,
              "connected latency under wakeup threshold");

  const char payload[] = "hello-reactor";
  ssize_t sent = send(fds[1], payload, sizeof(payload), 0);
  ok &= Check(sent == static_cast<ssize_t>(sizeof(payload)),
              "peer sends payload");
  ok &= Check(WaitUntil([&] { return got_data.load(std::memory_order_relaxed); },
                        std::chrono::milliseconds(500)),
              "data event received");

  reactor.Stop();
  worker_pool->Stop();

  if (fds[1] >= 0) {
    close(fds[1]);
  }

  return ok ? 0 : 1;
}
