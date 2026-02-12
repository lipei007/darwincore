#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <darwincore/network/protocol.h>

using namespace darwincore::network;

namespace {

std::atomic<bool> g_running{true};

void SignalHandler(int) {
  g_running.store(false, std::memory_order_relaxed);
}

bool ParseInt(const char *s, int &out) {
  if (s == nullptr) {
    return false;
  }
  char *end = nullptr;
  long v = std::strtol(s, &end, 10);
  if (end == s || *end != '\0' || v <= 0 || v > 10000000) {
    return false;
  }
  out = static_cast<int>(v);
  return true;
}

std::string Preview(const std::string &text, size_t max_len = 80) {
  if (text.size() <= max_len) {
    return text;
  }
  return text.substr(0, max_len) + "...";
}

bool ParsePort(const char *s, uint16_t &port) {
  int v = 0;
  if (!ParseInt(s, v) || v > 65535) {
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

bool SetNonBlocking(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

struct PendingPacket {
  std::vector<uint8_t> bytes;
  size_t offset{0};
};

struct Session {
  int index{0};
  int fd{-1};
  bool connected{false};
  bool connecting{false};
  bool failed{false};
  bool fatal_error{false};
  uint64_t next_message_id{1};
  uint64_t next_send_seq{1};
  std::chrono::steady_clock::time_point next_send_time{};
  std::chrono::steady_clock::time_point next_connect_time{};
  proto::Decoder decoder;
  std::deque<std::string> expected;
  std::deque<PendingPacket> outq;
};

void CloseSession(Session &s) {
  if (s.fd >= 0) {
    close(s.fd);
    s.fd = -1;
  }
}

void PrepareReconnect(Session &s,
                      std::chrono::steady_clock::time_point now,
                      std::chrono::milliseconds delay) {
  CloseSession(s);
  s.connected = false;
  s.connecting = false;
  s.failed = false;
  s.expected.clear();
  s.outq.clear();
  s.decoder.Reset();
  s.next_connect_time = now + delay;
}

void EnqueueMessage(Session &s, const std::string &payload) {
  auto frames = proto::Encoder::EncodeMessage(
      s.next_message_id++,
      reinterpret_cast<const uint8_t *>(payload.data()),
      payload.size(),
      false);
  auto packets = proto::Encoder::SerializeFrames(frames);
  for (auto &pkt : packets) {
    PendingPacket p;
    p.bytes = std::move(pkt);
    p.offset = 0;
    s.outq.push_back(std::move(p));
  }
}

void TryFlushOutput(Session &s) {
  while (!s.outq.empty()) {
    auto &p = s.outq.front();
    const uint8_t *data = p.bytes.data() + p.offset;
    const size_t left = p.bytes.size() - p.offset;
    ssize_t n = send(s.fd, data, left, MSG_NOSIGNAL);
    if (n > 0) {
      p.offset += static_cast<size_t>(n);
      if (p.offset >= p.bytes.size()) {
        s.outq.pop_front();
      }
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      return;
    }
    s.failed = true;
    return;
  }
}

void HandleReadable(Session &s) {
  uint8_t buf[8192];
  while (true) {
    const ssize_t n = recv(s.fd, buf, sizeof(buf), 0);
    if (n > 0) {
      try {
        s.decoder.Feed(buf, static_cast<size_t>(n));
        proto::MessageComplete msg;
        while (s.decoder.GetMessage(msg)) {
          const std::string got(reinterpret_cast<const char *>(msg.data.data()), msg.data.size());
          if (s.expected.empty()) {
            std::cerr << "[client-" << s.index << "] unexpected echo len=" << got.size()
                      << " msg=" << Preview(got) << std::endl;
            continue;
          }
          const std::string expected = s.expected.front();
          s.expected.pop_front();
          std::cout << "[client-" << s.index << "] recv len=" << got.size()
                    << " msg=" << Preview(got) << std::endl;
          if (got != expected) {
            std::cerr << "[client-" << s.index << "] mismatch expected_len=" << expected.size()
                      << " got_len=" << got.size()
                      << " expected=" << Preview(expected)
                      << " got=" << Preview(got) << std::endl;
            s.failed = true;
            return;
          }
        }
      } catch (const proto::ProtocolError &e) {
        std::cerr << "[client-" << s.index << "] protocol error: " << e.what() << std::endl;
        s.failed = true;
        return;
      }
      continue;
    }
    if (n == 0) {
      s.failed = true;
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    s.failed = true;
    return;
  }
}

std::string BuildPayload(int index, uint64_t seq, size_t target_size) {
  std::string base = "client-" + std::to_string(index) + " seq-" + std::to_string(seq);
  if (base.size() >= target_size) {
    return base.substr(0, target_size);
  }
  std::string out = base;
  out.push_back(' ');
  const char fill = static_cast<char>('a' + (index % 26));
  if (out.size() < target_size) {
    out.append(target_size - out.size(), fill);
  }
  return out;
}

} // namespace

int main(int argc, char **argv) {
  std::string host = "127.0.0.1";
  uint16_t port = 19090;
  int concurrent_clients = 1;
  int payload_bytes = 256;
  int send_interval_ms = 2000;

  if (argc >= 2) {
    host = argv[1];
  }
  if (argc >= 3 && !ParsePort(argv[2], port)) {
    std::cerr << "invalid port: " << argv[2] << std::endl;
    return 2;
  }
  if (argc >= 4 && !ParseInt(argv[3], concurrent_clients)) {
    std::cerr << "invalid concurrent_clients: " << argv[3] << std::endl;
    return 2;
  }
  if (argc >= 5 && !ParseInt(argv[4], payload_bytes)) {
    std::cerr << "invalid payload_bytes: " << argv[4] << std::endl;
    return 2;
  }
  if (argc >= 6 && !ParseInt(argv[5], send_interval_ms)) {
    std::cerr << "invalid send_interval_ms: " << argv[5] << std::endl;
    return 2;
  }
  if (payload_bytes < 16 || payload_bytes > 1024 * 1024) {
    std::cerr << "payload_bytes out of range [16, 1048576]" << std::endl;
    return 2;
  }
  if (send_interval_ms < 10 || send_interval_ms > 600000) {
    std::cerr << "send_interval_ms out of range [10, 600000]" << std::endl;
    return 2;
  }

  signal(SIGINT, SignalHandler);
  signal(SIGTERM, SignalHandler);

  rlim_t soft_limit = 0;
  rlim_t hard_limit = 0;
  const rlim_t required_fds = static_cast<rlim_t>(concurrent_clients + 256);
  if (!EnsureFdLimit(required_fds, soft_limit, hard_limit)) {
    std::cerr << "warning: failed to read/raise RLIMIT_NOFILE" << std::endl;
  } else if (soft_limit < required_fds) {
    std::cerr << "fd limit too low: soft=" << static_cast<unsigned long long>(soft_limit)
              << " hard=" << static_cast<unsigned long long>(hard_limit)
              << " required>=" << static_cast<unsigned long long>(required_fds)
              << ". reduce clients or raise ulimit -n." << std::endl;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "invalid host: " << host << std::endl;
    return 2;
  }

  std::vector<Session> sessions(static_cast<size_t>(concurrent_clients));
  std::unordered_map<int, size_t> fd_to_idx;
  fd_to_idx.reserve(static_cast<size_t>(concurrent_clients) * 2);
  const auto start_now = std::chrono::steady_clock::now();
  for (int i = 0; i < concurrent_clients; ++i) {
    Session &s = sessions[static_cast<size_t>(i)];
    s.index = i;
    // 分批启动连接，避免瞬时 connect 风暴导致大量拒绝
    s.next_connect_time = start_now + std::chrono::milliseconds(i % 200);
  }

  std::cout << "protocol_echo_client started, host=" << host
            << " port=" << port
            << " concurrent_clients=" << concurrent_clients
            << " payload_bytes=" << payload_bytes
            << " send_interval_ms=" << send_interval_ms
            << " (Ctrl+C to stop)" << std::endl;

  auto last_stat = std::chrono::steady_clock::now();
  while (g_running.load(std::memory_order_relaxed)) {
    const auto now = std::chrono::steady_clock::now();

    // 尝试发起/重试连接
    for (size_t i = 0; i < sessions.size(); ++i) {
      Session &s = sessions[i];
      if (s.fatal_error || s.connected || s.connecting || s.fd >= 0) {
        continue;
      }
      if (now < s.next_connect_time) {
        continue;
      }

      s.fd = socket(AF_INET, SOCK_STREAM, 0);
      if (s.fd < 0 || !SetNonBlocking(s.fd)) {
        std::cerr << "[client-" << s.index << "] socket create failed: " << strerror(errno) << std::endl;
        PrepareReconnect(s, now, std::chrono::milliseconds(1000));
        continue;
      }

      const int rc = connect(s.fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
      if (rc == 0) {
        s.connected = true;
        s.connecting = false;
        s.next_send_time = now;
        fd_to_idx[s.fd] = i;
        std::cout << "[client-connected] index=" << s.index << std::endl;
      } else if (errno == EINPROGRESS) {
        s.connecting = true;
        s.connected = false;
        fd_to_idx[s.fd] = i;
      } else {
        std::cerr << "[client-" << s.index << "] connect failed: " << strerror(errno) << std::endl;
        PrepareReconnect(s, now, std::chrono::milliseconds(500));
      }
    }

    std::vector<pollfd> pfds;
    pfds.reserve(fd_to_idx.size());
    for (const auto &kv : fd_to_idx) {
      pollfd p{};
      p.fd = kv.first;
      p.events = POLLIN | POLLERR | POLLHUP;
      const Session &s = sessions[kv.second];
      if (s.connecting || !s.outq.empty()) {
        p.events |= POLLOUT;
      }
      pfds.push_back(p);
    }

    const int timeout_ms = 100;
    const int n = poll(pfds.data(), pfds.size(), timeout_ms);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "poll failed: " << strerror(errno) << std::endl;
      break;
    }

    for (auto &p : pfds) {
      auto it = fd_to_idx.find(p.fd);
      if (it == fd_to_idx.end()) {
        continue;
      }
      Session &s = sessions[it->second];

      if (s.connecting && (p.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL))) {
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &so_error, &len) == 0 && so_error == 0) {
          s.connected = true;
          s.connecting = false;
          s.next_send_time = now;
          std::cout << "[client-connected] index=" << s.index << std::endl;
        } else {
          s.failed = true;
          std::cerr << "[client-" << s.index << "] connect failed: " << strerror(so_error) << std::endl;
        }
        continue;
      }

      if (s.connected && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        s.failed = true;
      }

      if (!s.failed && s.connected && (p.revents & POLLIN)) {
        HandleReadable(s);
      }
      if (!s.failed && s.connected && (p.revents & POLLOUT) && !s.outq.empty()) {
        TryFlushOutput(s);
      }
    }

    for (auto &s : sessions) {
      if (s.failed || !s.connected) {
        continue;
      }
      if (now >= s.next_send_time) {
        const uint64_t seq = s.next_send_seq++;
        const std::string payload = BuildPayload(s.index, seq, static_cast<size_t>(payload_bytes));
        s.expected.push_back(payload);
        try {
          EnqueueMessage(s, payload);
          TryFlushOutput(s);
          std::cout << "[client-" << s.index << "] send len=" << payload.size()
                    << " msg=" << Preview(payload) << std::endl;
        } catch (const proto::ProtocolError &e) {
          std::cerr << "[client-" << s.index << "] encode error: " << e.what() << std::endl;
          s.failed = true;
          s.fatal_error = true;
        }
        s.next_send_time = now + std::chrono::milliseconds(send_interval_ms);
      }
    }

    for (auto &s : sessions) {
      if (s.failed && s.fd >= 0) {
        fd_to_idx.erase(s.fd);
        if (s.fatal_error) {
          CloseSession(s);
          s.connected = false;
          s.connecting = false;
        } else {
          PrepareReconnect(s, now, std::chrono::milliseconds(500));
        }
      }
    }

    if (now - last_stat >= std::chrono::seconds(2)) {
      int connected = 0;
      int connecting = 0;
      int retrying = 0;
      int fatal = 0;
      for (const auto &s : sessions) {
        connected += s.connected ? 1 : 0;
        connecting += s.connecting ? 1 : 0;
        retrying += (!s.connected && !s.connecting && !s.fatal_error) ? 1 : 0;
        fatal += s.fatal_error ? 1 : 0;
      }
      std::cout << "[client-stats] connected=" << connected
                << " connecting=" << connecting
                << " retrying=" << retrying
                << " fatal=" << fatal << std::endl;
      last_stat = now;
    }
  }

  for (auto &s : sessions) {
    CloseSession(s);
  }
  std::cout << "client stopped" << std::endl;
  return 0;
}
