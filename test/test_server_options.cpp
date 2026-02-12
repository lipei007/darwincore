#include <iostream>

#include <darwincore/network/server.h>

using namespace darwincore::network;

namespace {
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
  Server::Options opts;
  opts.reactor_count = 2;
  opts.worker_count = 3;
  opts.worker_queue_size = 256;

  Server server(opts);
  const auto stats = server.GetStatistics();

  bool ok = true;
  ok &= Check(!stats.is_running, "server is not running after options ctor");
  ok &= Check(stats.worker_submit_failures == 0,
              "worker_submit_failures default to zero");
  ok &= Check(stats.reactors.empty(), "reactor stats empty before start");
  return ok ? 0 : 1;
}
