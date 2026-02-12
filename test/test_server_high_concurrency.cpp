#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstring>
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

struct Config {
  int client_count = 24;
  int messages_per_client = 24;
  size_t payload_size = 256;
  int send_p99_ms_max = 100;
  int timeout_seconds = 20;
};

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

bool ParseInt(const char *s, int &out) {
  if (s == nullptr) {
    return false;
  }
  char *end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v <= 0 || v > 1000000) {
    return false;
  }
  out = static_cast<int>(v);
  return true;
}

uint64_t Percentile(std::vector<uint64_t> values, double p) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const size_t idx = static_cast<size_t>(
      std::min<double>(values.size() - 1, p * (values.size() - 1)));
  return values[idx];
}

} // namespace

int main(int argc, char **argv) {
  Config cfg;
  if (argc >= 2 && !ParseInt(argv[1], cfg.client_count)) {
    std::cerr << "invalid client_count" << std::endl;
    return 2;
  }
  if (argc >= 3 && !ParseInt(argv[2], cfg.messages_per_client)) {
    std::cerr << "invalid messages_per_client" << std::endl;
    return 2;
  }
  if (argc >= 4) {
    int v = 0;
    if (!ParseInt(argv[3], v)) {
      std::cerr << "invalid payload_size" << std::endl;
      return 2;
    }
    cfg.payload_size = static_cast<size_t>(v);
  }
  if (argc >= 5 && !ParseInt(argv[4], cfg.send_p99_ms_max)) {
    std::cerr << "invalid send_p99_ms_max" << std::endl;
    return 2;
  }

  bool ok = true;
  const auto bench_start = std::chrono::steady_clock::now();

  Server::Options options;
  options.reactor_count = 2;
  options.worker_count = 4;
  options.worker_queue_size = 4096;
  Server server(options);

  std::atomic<uint64_t> server_bytes_in{0};
  std::atomic<uint64_t> server_echo_failures{0};

  server.SetOnMessage([&](uint64_t conn_id, const std::vector<uint8_t> &data) {
    server_bytes_in.fetch_add(data.size(), std::memory_order_relaxed);
    if (!server.SendData(conn_id, data.data(), data.size())) {
      server_echo_failures.fetch_add(1, std::memory_order_relaxed);
    }
  });
  server.SetOnClientConnected([&](const ConnectionInformation &) {});
  server.SetOnClientDisconnected([&](uint64_t) {});
  server.SetOnConnectionError([&](uint64_t, NetworkError, const std::string &) {});

  uint16_t port = 19900;
  bool started = false;
  for (int i = 0; i < 64 && !started; ++i) {
    started = server.StartIPv4("127.0.0.1", port + i);
    if (started) {
      port += i;
      break;
    }
  }
  ok &= Check(started, "server starts on an available local port");
  if (!ok) {
    return 1;
  }

  struct ClientState {
    std::unique_ptr<Client> client;
    std::atomic<bool> connected{false};
    std::atomic<bool> done{false};
    std::atomic<bool> failed{false};
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_recv{0};
    std::vector<uint64_t> send_lat_us;
    std::thread sender;
  };

  std::vector<std::unique_ptr<ClientState>> states;
  states.reserve(cfg.client_count);

  for (int i = 0; i < cfg.client_count; ++i) {
    auto state = std::make_unique<ClientState>();
    state->client = std::make_unique<Client>();
    auto *s = state.get();

    s->client->SetOnConnected([s](const ConnectionInformation &) {
      s->connected.store(true, std::memory_order_relaxed);
    });
    s->client->SetOnMessage([s, &cfg](const std::vector<uint8_t> &data) {
      const uint64_t now = s->bytes_recv.fetch_add(data.size(), std::memory_order_relaxed) + data.size();
      const uint64_t expected = static_cast<uint64_t>(cfg.messages_per_client) * cfg.payload_size;
      if (now >= expected) {
        s->done.store(true, std::memory_order_relaxed);
      }
    });
    s->client->SetOnError([s](NetworkError, const std::string &) {
      s->failed.store(true, std::memory_order_relaxed);
      s->done.store(true, std::memory_order_relaxed);
    });

    states.push_back(std::move(state));
  }

  for (auto &st : states) {
    if (!st->client->ConnectIPv4("127.0.0.1", port)) {
      st->failed.store(true, std::memory_order_relaxed);
      st->done.store(true, std::memory_order_relaxed);
    }
  }

  for (size_t i = 0; i < states.size(); ++i) {
    auto &st = states[i];
    st->sender = std::thread([i, &st, &cfg] {
      const bool is_connected = WaitUntil(
          [&] { return st->connected.load(std::memory_order_relaxed); },
          std::chrono::seconds(3));
      if (!is_connected) {
        st->failed.store(true, std::memory_order_relaxed);
        st->done.store(true, std::memory_order_relaxed);
        return;
      }

      std::vector<uint8_t> payload(cfg.payload_size, static_cast<uint8_t>('a' + (i % 26)));
      st->send_lat_us.reserve(static_cast<size_t>(cfg.messages_per_client));
      for (int n = 0; n < cfg.messages_per_client; ++n) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!st->client->SendData(payload.data(), payload.size(), 2000)) {
          st->failed.store(true, std::memory_order_relaxed);
          st->done.store(true, std::memory_order_relaxed);
          return;
        }
        const auto t1 = std::chrono::steady_clock::now();
        st->send_lat_us.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()));
        st->bytes_sent.fetch_add(payload.size(), std::memory_order_relaxed);
      }
    });
  }

  ok &= Check(WaitUntil(
                  [&] {
                    for (const auto &st : states) {
                      if (!st->done.load(std::memory_order_relaxed) &&
                          !st->failed.load(std::memory_order_relaxed)) {
                        return false;
                      }
                    }
                    return true;
                  },
                  std::chrono::seconds(cfg.timeout_seconds)),
              "all clients complete echo workload");

  uint64_t total_client_sent = 0;
  uint64_t total_client_recv = 0;
  uint64_t failed_clients = 0;
  std::vector<uint64_t> all_send_lat_us;
  all_send_lat_us.reserve(static_cast<size_t>(cfg.client_count) *
                          static_cast<size_t>(cfg.messages_per_client));
  for (auto &st : states) {
    if (st->sender.joinable()) {
      st->sender.join();
    }
    st->client->Disconnect();
    total_client_sent += st->bytes_sent.load(std::memory_order_relaxed);
    total_client_recv += st->bytes_recv.load(std::memory_order_relaxed);
    all_send_lat_us.insert(all_send_lat_us.end(), st->send_lat_us.begin(),
                           st->send_lat_us.end());
    if (st->failed.load(std::memory_order_relaxed)) {
      ++failed_clients;
    }
  }

  ok &= Check(failed_clients == 0, "no client failed during concurrent workload");
  ok &= Check(total_client_recv >= total_client_sent,
              "echo received bytes cover sent bytes");
  ok &= Check(server_echo_failures.load(std::memory_order_relaxed) == 0,
              "server echo path has no SendData failures");

  ok &= Check(WaitUntil(
                  [&] { return server.GetStatistics().active_connections == 0; },
                  std::chrono::seconds(3)),
              "active connection count returns to zero");

  const auto stats = server.GetStatistics();
  ok &= Check(stats.total_bytes_received >= total_client_sent,
              "server total_bytes_received covers client sent bytes");
  ok &= Check(stats.total_bytes_sent >= total_client_sent,
              "server total_bytes_sent covers echoed bytes");
  ok &= Check(stats.worker_submit_failures == 0,
              "worker submit failures remain zero");

  uint64_t reactor_send_requests = 0;
  uint64_t reactor_send_failures = 0;
  for (const auto &r : stats.reactors) {
    reactor_send_requests += r.total_send_requests;
    reactor_send_failures += r.total_send_failures;
  }
  ok &= Check(reactor_send_requests == stats.total_messages_sent,
              "reactor send requests match server send message counter");
  ok &= Check(reactor_send_requests > 0,
              "reactor send requests are non-zero");
  ok &= Check(reactor_send_failures == 0,
              "reactor send failure counter remains zero");

  const uint64_t p50_us = Percentile(all_send_lat_us, 0.50);
  const uint64_t p99_us = Percentile(all_send_lat_us, 0.99);
  const auto bench_end = std::chrono::steady_clock::now();
  const double elapsed_sec =
      std::chrono::duration_cast<std::chrono::milliseconds>(bench_end - bench_start)
          .count() /
      1000.0;
  const double mb_sent = static_cast<double>(total_client_sent) / 1024.0 / 1024.0;
  const double throughput_mb_s = elapsed_sec > 0.0 ? (mb_sent / elapsed_sec) : 0.0;

  std::cout << "[METRIC] clients=" << cfg.client_count
            << " messages_per_client=" << cfg.messages_per_client
            << " payload=" << cfg.payload_size << "B"
            << " elapsed=" << elapsed_sec << "s"
            << " send_p50=" << (p50_us / 1000.0) << "ms"
            << " send_p99=" << (p99_us / 1000.0) << "ms"
            << " throughput=" << throughput_mb_s << "MB/s"
            << std::endl;

  ok &= Check((p99_us / 1000.0) <= static_cast<double>(cfg.send_p99_ms_max),
              "send latency p99 is within threshold");

  server.Stop();
  ok &= Check(!server.GetStatistics().is_running, "server stops cleanly");

  return ok ? 0 : 1;
}
