#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>

#include <darwincore/network/event.h>
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
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
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
  WorkerPool pool(1, 64);
  std::atomic<int> callback_count{0};

  pool.SetEventCallback([&](const NetworkEvent &event) {
    if (event.type == NetworkEventType::kData && event.connection_id == 42) {
      callback_count.fetch_add(1, std::memory_order_relaxed);
    }
  });

  bool ok = true;
  ok &= Check(pool.Start(), "worker pool starts");

  NetworkEvent event(NetworkEventType::kData, 42);
  event.payload = {1, 2, 3, 4};
  pool.SubmitEvent(event);

  ok &= Check(WaitUntil(
                  [&] { return callback_count.load(std::memory_order_relaxed) == 1; },
                  std::chrono::seconds(1)),
              "submitted event is dispatched");

  pool.Stop();
  ok &= Check(callback_count.load(std::memory_order_relaxed) == 1,
              "callback count remains stable after stop");
  ok &= Check(pool.GetSubmitFailures() == 0,
              "submit failure stays zero on healthy path");

  NetworkEvent late_event(NetworkEventType::kData, 42);
  ok &= Check(!pool.SubmitEvent(late_event),
              "submit fails after pool stop");
  ok &= Check(pool.GetSubmitFailures() >= 1,
              "submit failure counter increments");

  return ok ? 0 : 1;
}
