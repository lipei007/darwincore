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
  Server server;
  const auto stats = server.GetStatistics();

  bool ok = true;
  ok &= Check(!stats.is_running, "server is not running by default");
  ok &= Check(stats.total_connections == 0, "default total_connections is zero");
  ok &= Check(stats.active_connections == 0, "default active_connections is zero");
  ok &= Check(stats.total_messages_received == 0,
              "default total_messages_received is zero");
  ok &= Check(stats.total_messages_sent == 0,
              "default total_messages_sent is zero");
  ok &= Check(stats.total_bytes_received == 0,
              "default total_bytes_received is zero");
  ok &= Check(stats.total_bytes_sent == 0, "default total_bytes_sent is zero");
  ok &= Check(stats.total_errors == 0, "default total_errors is zero");
  ok &= Check(stats.worker_submit_failures == 0,
              "default worker_submit_failures is zero");
  ok &= Check(stats.worker_total_queue_size == 0,
              "default worker_total_queue_size is zero");
  ok &= Check(stats.reactors.empty(), "default reactor statistics are empty");

  Server::ReactorStatistics r;
  ok &= Check(r.total_add_requests == 0, "default reactor add requests is zero");
  ok &= Check(r.total_add_failures == 0, "default reactor add failures is zero");
  ok &= Check(r.total_remove_requests == 0, "default reactor remove requests is zero");
  ok &= Check(r.total_remove_failures == 0, "default reactor remove failures is zero");
  ok &= Check(r.total_send_requests == 0, "default reactor send requests is zero");
  ok &= Check(r.total_send_failures == 0, "default reactor send failures is zero");

  return ok ? 0 : 1;
}
