#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <darwincore/network/client.h>
#include <darwincore/network/server.h>

using namespace darwincore::network;

namespace {

constexpr int kRounds = 8;
constexpr int kClientsPerRound = 12;
constexpr int kMessagesPerClient = 12;
constexpr size_t kPayloadSize = 128;

bool Check(bool cond, const char *msg) {
  if (!cond) {
    std::cerr << "[FAIL] " << msg << std::endl;
    return false;
  }
  std::cout << "[PASS] " << msg << std::endl;
  return true;
}

bool WaitUntil(const std::function<bool()> &pred,
               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return pred();
}

struct ClientRun {
  std::unique_ptr<Client> client;
  std::atomic<bool> connected{false};
  std::atomic<bool> done{false};
  std::atomic<bool> failed{false};
  std::atomic<uint64_t> recv_bytes{0};
  std::thread sender;
};

} // namespace

int main() {
  bool ok = true;

  Server::Options options;
  options.reactor_count = 2;
  options.worker_count = 4;
  options.worker_queue_size = 4096;
  Server server(options);

  std::atomic<uint64_t> echo_send_failures{0};
  server.SetOnClientConnected([&](const ConnectionInformation &) {});
  server.SetOnClientDisconnected([&](uint64_t) {});
  server.SetOnConnectionError([&](uint64_t, NetworkError, const std::string &) {});
  server.SetOnMessage([&](uint64_t conn_id, const std::vector<uint8_t> &data) {
    if (!server.SendData(conn_id, data.data(), data.size())) {
      echo_send_failures.fetch_add(1, std::memory_order_relaxed);
    }
  });

  uint16_t port = 20080;
  bool started = false;
  for (int i = 0; i < 64 && !started; ++i) {
    started = server.StartIPv4("127.0.0.1", static_cast<uint16_t>(port + i));
    if (started) {
      port = static_cast<uint16_t>(port + i);
      break;
    }
  }
  ok &= Check(started, "server starts for soak test");
  if (!ok) {
    return 1;
  }

  for (int round = 0; round < kRounds; ++round) {
    std::vector<std::unique_ptr<ClientRun>> runs;
    runs.reserve(kClientsPerRound);

    for (int i = 0; i < kClientsPerRound; ++i) {
      auto run = std::make_unique<ClientRun>();
      auto *r = run.get();
      r->client = std::make_unique<Client>();

      r->client->SetOnConnected([r](const ConnectionInformation &) {
        r->connected.store(true, std::memory_order_relaxed);
      });
      r->client->SetOnMessage([r](const std::vector<uint8_t> &data) {
        const uint64_t now = r->recv_bytes.fetch_add(data.size(), std::memory_order_relaxed) + data.size();
        if (now >= static_cast<uint64_t>(kMessagesPerClient) * kPayloadSize) {
          r->done.store(true, std::memory_order_relaxed);
        }
      });
      r->client->SetOnError([r](NetworkError, const std::string &) {
        r->failed.store(true, std::memory_order_relaxed);
        r->done.store(true, std::memory_order_relaxed);
      });

      if (!r->client->ConnectIPv4("127.0.0.1", port)) {
        r->failed.store(true, std::memory_order_relaxed);
        r->done.store(true, std::memory_order_relaxed);
      }

      runs.push_back(std::move(run));
    }

    for (int i = 0; i < kClientsPerRound; ++i) {
      auto &run = runs[static_cast<size_t>(i)];
      run->sender = std::thread([&, i] {
        auto *r = runs[static_cast<size_t>(i)].get();
        if (!WaitUntil([&] { return r->connected.load(std::memory_order_relaxed); },
                       std::chrono::seconds(3))) {
          r->failed.store(true, std::memory_order_relaxed);
          r->done.store(true, std::memory_order_relaxed);
          return;
        }

        std::vector<uint8_t> payload(kPayloadSize, static_cast<uint8_t>('A' + (i % 26)));
        for (int n = 0; n < kMessagesPerClient; ++n) {
          if (!r->client->SendData(payload.data(), payload.size(), 2000)) {
            r->failed.store(true, std::memory_order_relaxed);
            r->done.store(true, std::memory_order_relaxed);
            return;
          }
        }
      });
    }

    ok &= Check(WaitUntil([&] {
                    for (const auto &run : runs) {
                      if (!run->done.load(std::memory_order_relaxed) &&
                          !run->failed.load(std::memory_order_relaxed)) {
                        return false;
                      }
                    }
                    return true;
                  }, std::chrono::seconds(15)),
                "round workload completes");

    uint64_t round_failed = 0;
    for (auto &run : runs) {
      if (run->sender.joinable()) {
        run->sender.join();
      }
      run->client->Disconnect();
      if (run->failed.load(std::memory_order_relaxed)) {
        ++round_failed;
      }
    }
    ok &= Check(round_failed == 0, "no client failure in round");
    ok &= Check(WaitUntil([&] { return server.GetStatistics().active_connections == 0; },
                          std::chrono::seconds(3)),
                "active_connections returns to zero after round");
  }

  const auto stats = server.GetStatistics();
  ok &= Check(stats.worker_submit_failures == 0,
              "worker submit failures stay zero in soak");
  ok &= Check(echo_send_failures.load(std::memory_order_relaxed) == 0,
              "server echo send failures stay zero in soak");

  uint64_t total_send_failures = 0;
  uint64_t total_dispatch_failures = 0;
  for (const auto &r : stats.reactors) {
    total_send_failures += r.total_send_failures;
    total_dispatch_failures += r.total_dispatch_failures;
  }
  ok &= Check(total_send_failures == 0, "reactor send failures stay zero in soak");
  ok &= Check(total_dispatch_failures == 0, "reactor dispatch failures stay zero in soak");

  server.Stop();
  ok &= Check(!server.GetStatistics().is_running, "server stops cleanly after soak");

  return ok ? 0 : 1;
}
