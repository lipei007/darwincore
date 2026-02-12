#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sys/resource.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <darwincore/network/protocol.h>
#include <darwincore/network/server.h>

using namespace darwincore::network;

namespace {

std::atomic<bool> g_running{true};

void SignalHandler(int) {
  g_running.store(false, std::memory_order_relaxed);
}

struct ConnectionState {
  proto::Decoder decoder;
  uint64_t next_reply_id{1};
  std::mutex mutex;
};

bool ParsePort(const char *s, uint16_t &port) {
  if (s == nullptr) {
    return false;
  }
  char *end = nullptr;
  const long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v <= 0 || v > 65535) {
    return false;
  }
  port = static_cast<uint16_t>(v);
  return true;
}

bool EnsureFdLimit(rlim_t target, rlim_t &soft_after, rlim_t &hard_after) {
  struct rlimit lim{};
  if (getrlimit(RLIMIT_NOFILE, &lim) != 0) {
    return false;
  }
  if (lim.rlim_cur < target) {
    rlim_t desired = target;
    if (lim.rlim_max != RLIM_INFINITY && desired > lim.rlim_max) {
      desired = lim.rlim_max;
    }
    lim.rlim_cur = desired;
    (void)setrlimit(RLIMIT_NOFILE, &lim);
  }
  if (getrlimit(RLIMIT_NOFILE, &lim) != 0) {
    return false;
  }
  soft_after = lim.rlim_cur;
  hard_after = lim.rlim_max;
  return true;
}

std::string Preview(const std::string &text, size_t max_len = 80) {
  if (text.size() <= max_len) {
    return text;
  }
  return text.substr(0, max_len) + "...";
}

} // namespace

int main(int argc, char **argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 19090;

  if (argc >= 2) {
    host = argv[1];
  }
  if (argc >= 3 && !ParsePort(argv[2], port)) {
    std::cerr << "invalid port: " << argv[2] << std::endl;
    return 2;
  }

  std::signal(SIGINT, SignalHandler);
  std::signal(SIGTERM, SignalHandler);

  rlim_t soft_limit = 0;
  rlim_t hard_limit = 0;
  const rlim_t recommended = 20000;
  if (!EnsureFdLimit(recommended, soft_limit, hard_limit)) {
    std::cerr << "warning: failed to read/raise RLIMIT_NOFILE" << std::endl;
  } else {
    std::cout << "[fd-limit] soft=" << static_cast<unsigned long long>(soft_limit)
              << " hard=" << static_cast<unsigned long long>(hard_limit) << std::endl;
  }

  Server::Options options;
  options.reactor_count = 0;
  options.worker_count = 4;
  options.worker_queue_size = 10000;
  Server server(options);

  std::mutex states_mu;
  std::unordered_map<uint64_t, std::shared_ptr<ConnectionState>> states;

  server.SetOnClientConnected([&](const ConnectionInformation &info) {
    auto state = std::make_shared<ConnectionState>();
    std::lock_guard<std::mutex> lock(states_mu);
    states[info.connection_id] = std::move(state);
    std::cout << "[connected] conn_id=" << info.connection_id
              << " peer=" << info.peer_address << ":" << info.peer_port << std::endl;
  });

  server.SetOnClientDisconnected([&](uint64_t conn_id) {
    std::lock_guard<std::mutex> lock(states_mu);
    states.erase(conn_id);
    std::cout << "[disconnected] conn_id=" << conn_id << std::endl;
  });

  server.SetOnConnectionError([&](uint64_t conn_id, NetworkError err, const std::string &msg) {
    std::cerr << "[error] conn_id=" << conn_id << " err=" << static_cast<int>(err)
              << " msg=" << msg << std::endl;
  });

  server.SetOnMessage([&](uint64_t conn_id, const std::vector<uint8_t> &data) {
    std::shared_ptr<ConnectionState> state;
    {
      std::lock_guard<std::mutex> lock(states_mu);
      auto it = states.find(conn_id);
      if (it == states.end()) {
        return;
      }
      state = it->second;
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    try {
      state->decoder.Feed(data.data(), data.size());

      proto::MessageComplete msg;
      while (state->decoder.GetMessage(msg)) {
        const std::string text(reinterpret_cast<const char *>(msg.data.data()), msg.data.size());
        std::cout << "[recv] conn_id=" << conn_id
                  << " len=" << text.size()
                  << " msg=" << Preview(text) << std::endl;

        auto frames = proto::Encoder::EncodeMessage(
            state->next_reply_id++, msg.data.data(), msg.data.size(), false);
        auto packets = proto::Encoder::SerializeFrames(frames);
        for (const auto &pkt : packets) {
          if (!server.SendData(conn_id, pkt.data(), pkt.size())) {
            std::cerr << "[send-fail] conn_id=" << conn_id << std::endl;
            break;
          }
        }
      }
    } catch (const proto::ProtocolError &e) {
      std::cerr << "[protocol-error] conn_id=" << conn_id << " " << e.what() << std::endl;
    }
  });

  if (!server.StartIPv4(host, port)) {
    std::cerr << "failed to start server at " << host << ":" << port << std::endl;
    return 1;
  }

  std::cout << "protocol_echo_server listening at " << host << ":" << port << std::endl;
  std::cout << "press Ctrl+C to stop" << std::endl;

  std::thread stats_thread([&] {
    uint64_t last_recv_msg = 0;
    uint64_t last_sent_msg = 0;
    uint64_t last_recv_bytes = 0;
    uint64_t last_sent_bytes = 0;
    const auto start_tp = std::chrono::steady_clock::now();
    auto last_tp = start_tp;

    while (g_running.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      if (!g_running.load(std::memory_order_relaxed)) {
        break;
      }
      const auto stats = server.GetStatistics();
      const auto now = std::chrono::steady_clock::now();
      const double secs = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tp).count() / 1000.0;

      const uint64_t recv_msg_delta = stats.total_messages_received - last_recv_msg;
      const uint64_t sent_msg_delta = stats.total_messages_sent - last_sent_msg;
      const uint64_t recv_bytes_delta = stats.total_bytes_received - last_recv_bytes;
      const uint64_t sent_bytes_delta = stats.total_bytes_sent - last_sent_bytes;

      const double recv_qps = secs > 0.0 ? static_cast<double>(recv_msg_delta) / secs : 0.0;
      const double sent_qps = secs > 0.0 ? static_cast<double>(sent_msg_delta) / secs : 0.0;
      const double recv_mb_s = secs > 0.0 ? static_cast<double>(recv_bytes_delta) / 1024.0 / 1024.0 / secs : 0.0;
      const double sent_mb_s = secs > 0.0 ? static_cast<double>(sent_bytes_delta) / 1024.0 / 1024.0 / secs : 0.0;
      const double up_secs =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - start_tp).count() / 1000.0;
      const double recv_qps_avg =
          up_secs > 0.0 ? static_cast<double>(stats.total_messages_received) / up_secs : 0.0;
      const double sent_qps_avg =
          up_secs > 0.0 ? static_cast<double>(stats.total_messages_sent) / up_secs : 0.0;
      const double recv_mb_s_avg =
          up_secs > 0.0 ? static_cast<double>(stats.total_bytes_received) / 1024.0 / 1024.0 / up_secs : 0.0;
      const double sent_mb_s_avg =
          up_secs > 0.0 ? static_cast<double>(stats.total_bytes_sent) / 1024.0 / 1024.0 / up_secs : 0.0;

      last_recv_msg = stats.total_messages_received;
      last_sent_msg = stats.total_messages_sent;
      last_recv_bytes = stats.total_bytes_received;
      last_sent_bytes = stats.total_bytes_sent;
      last_tp = now;

      std::cout << std::fixed << std::setprecision(2);
      std::cout << "[stats] active=" << stats.active_connections
                << " total_conn=" << stats.total_connections
                << " recv_msg=" << stats.total_messages_received
                << " sent_msg=" << stats.total_messages_sent
                << " recv_bytes=" << stats.total_bytes_received
                << " sent_bytes=" << stats.total_bytes_sent
                << " recv_qps=" << recv_qps
                << " sent_qps=" << sent_qps
                << " recv_mb_s=" << recv_mb_s
                << " sent_mb_s=" << sent_mb_s
                << " recv_qps_avg=" << recv_qps_avg
                << " sent_qps_avg=" << sent_qps_avg
                << " recv_mb_s_avg=" << recv_mb_s_avg
                << " sent_mb_s_avg=" << sent_mb_s_avg
                << " worker_submit_fail=" << stats.worker_submit_failures
                << std::endl;
    }
  });

  while (g_running.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  server.Stop();
  if (stats_thread.joinable()) {
    stats_thread.join();
  }
  std::cout << "server stopped" << std::endl;
  return 0;
}
