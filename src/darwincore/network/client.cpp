#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <csignal>
#include <mutex>
#include <atomic>
#include <chrono>
#include <thread>

#include <darwincore/network/logger.h>
#include "socket_helper.h"
#include "reactor.h"
#include "worker_pool.h"
#include <darwincore/network/client.h>

namespace darwincore::network
{

  class Client::Impl
  {
  public:
    Impl();
    ~Impl();

    bool ConnectIPv4(const std::string &, uint16_t);
    bool ConnectIPv6(const std::string &, uint16_t);
    bool ConnectUnixDomain(const std::string &);
    bool GracefulShutdown(int timeout_ms);
    void Disconnect();

    bool SendData(const uint8_t *, size_t);
    bool IsConnected() const;

    void SetOnConnected(OnConnectedCallback cb) { on_connected_ = cb;}
    void SetOnMessage(OnMessageCallback cb) { on_message_ = cb;}
    void SetOnDisconnected(OnDisconnectedCallback cb) { on_disconnected_ = cb;}
    void SetOnError(OnErrorCallback cb) { on_error_ = cb;}

  private:
    enum class State
    {
      kDisconnected,
      kConnecting,
      kConnected,
      kClosing
    };

    // 发送背压水位标记（字节）
    static constexpr size_t SEND_HIGH_WATER_MARK = 4 * 1024 * 1024;  // 4MB

    bool ConnectInternal(int fd, const sockaddr *, socklen_t, bool is_tcp);
    bool InitReactor();

    void Cleanup();
    void OnNetworkEvent(const NetworkEvent &);

  private:
    std::shared_ptr<WorkerPool> worker_pool_;
    std::unique_ptr<Reactor> reactor_;

    std::atomic<uint64_t> connection_id_{0};
    std::atomic<State> state_{State::kDisconnected};

    sockaddr_storage peer_{};

    std::mutex cb_mutex_;
    OnConnectedCallback on_connected_;
    OnMessageCallback on_message_;
    OnDisconnectedCallback on_disconnected_;
    OnErrorCallback on_error_;
  };

  /* ================= Impl ================= */

  Client::Impl::Impl()
  {
    static std::once_flag f;
    std::call_once(f, []
                   { signal(SIGPIPE, SIG_IGN); });
  }

  Client::Impl::~Impl()
  {
    Disconnect();
  }

  bool Client::Impl::InitReactor()
  {
    if (reactor_)
      return true;

    worker_pool_ = std::make_shared<WorkerPool>(1);
    if (!worker_pool_->Start())
    {
      return false;
    }

    worker_pool_->SetEventCallback(
        [this](const NetworkEvent &ev) { 
          OnNetworkEvent(ev); 
        });

    reactor_ = std::make_unique<Reactor>(0, worker_pool_);
    return reactor_->Start();
  }

  bool Client::Impl::ConnectInternal(
      int fd,
      const sockaddr *addr,
      socklen_t len,
      bool is_tcp)
  {

    if (state_.exchange(State::kConnecting) != State::kDisconnected)
    {
      close(fd);
      return false;
    }

    SocketHelper::SetNonBlocking(fd);

    if (is_tcp)
    {
      int flag = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
      setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
    }

    if (connect(fd, addr, len) < 0 && errno != EINPROGRESS)
    {
      close(fd);
      state_.store(State::kDisconnected);
      return false;
    }

    memcpy(&peer_, addr, len);

    if (!InitReactor())
    {
      close(fd);
      state_.store(State::kDisconnected);
      return false;
    }

    bool success = reactor_->AddConnection(fd, peer_);
    if (!success)
    {
      close(fd);
      state_.store(State::kDisconnected);
      return false;
    }

    return true;
  }

  bool Client::Impl::ConnectIPv4(const std::string &host, uint16_t port)
  {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
      return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    return fd >= 0 &&
           ConnectInternal(fd, (sockaddr *)&addr, sizeof(addr), true);
  }

  bool Client::Impl::ConnectIPv6(const std::string &host, uint16_t port)
  {
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(port);
    if (inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr) != 1)
      return false;

    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    return fd >= 0 &&
           ConnectInternal(fd, (sockaddr *)&addr, sizeof(addr), true);
  }

  bool Client::Impl::ConnectUnixDomain(const std::string &path)
  {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    return fd >= 0 &&
           ConnectInternal(fd, (sockaddr *)&addr, sizeof(addr), false);
  }

  bool Client::Impl::GracefulShutdown(int timeout_ms)
  {
    State expected = State::kConnected;
    if (!state_.compare_exchange_strong(expected, State::kClosing) &&
        expected != State::kConnecting)
      return false;

    // 等待SendBuffer清空
    auto start = std::chrono::steady_clock::now();
    uint64_t conn_id = connection_id_.load();

    while (reactor_)
    {
      size_t buffer_size = reactor_->GetSendBufferSize(conn_id);
      if (buffer_size == 0)
      {
        NW_LOG_INFO("[Client] SendBuffer已清空，准备关闭连接");
        break;
      }

      // 检查超时
      if (timeout_ms > 0)
      {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms)
        {
          NW_LOG_WARNING("[Client] GracefulShutdown超时: buffer_size=" << buffer_size);
          return false;
        }
      }

      // 短暂等待
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    Cleanup();
    state_.store(State::kDisconnected);
    return true;
  }

  void Client::Impl::Disconnect()
  {
    State expected = State::kConnected;
    if (!state_.compare_exchange_strong(expected, State::kClosing) &&
        expected != State::kConnecting)
      return;

    Cleanup();
    state_.store(State::kDisconnected);
  }

  void Client::Impl::Cleanup()
  {
    if (reactor_)
    {
      reactor_->Stop();
      reactor_.reset();
    }
    if (worker_pool_)
    {
      worker_pool_->Stop();
      worker_pool_.reset();
    }
    connection_id_.store(0);
  }

  bool Client::Impl::SendData(const uint8_t *data, size_t size)
  {
    if (state_.load() != State::kConnected || !reactor_)
      return false;

    // 发送背压检查：如果发送缓冲区超过高水位，拒绝发送
    size_t buffer_size = reactor_->GetSendBufferSize(connection_id_.load());
    if (buffer_size > SEND_HIGH_WATER_MARK)
    {
      NW_LOG_WARNING("[Client] 发送背压触发: buffer_size=" << buffer_size
                     << " > HIGH_WATER_MARK=" << SEND_HIGH_WATER_MARK);
      return false;
    }

    return reactor_->SendData(connection_id_.load(), data, size);
  }

  bool Client::Impl::IsConnected() const
  {
    return state_.load() == State::kConnected;
  }

  void Client::Impl::OnNetworkEvent(const NetworkEvent &ev)
  {
    switch (ev.type)
    {
    case NetworkEventType::kConnected:
    {
      state_.store(State::kConnected);
      connection_id_.store(ev.connection_id);
      OnConnectedCallback cb;
      {
        std::lock_guard lk(cb_mutex_);
        cb = on_connected_;
      }
      if (cb && ev.connection_info)
        cb(*ev.connection_info);
      break;
    }

    case NetworkEventType::kData:
    {
      OnMessageCallback cb;
      {
        std::lock_guard lk(cb_mutex_);
        cb = on_message_;
      }
      if (cb)
        cb(ev.payload);
      break;
    }

    case NetworkEventType::kDisconnected:
    case NetworkEventType::kError:
    {
      state_.store(State::kDisconnected);
      connection_id_.store(0);
      OnDisconnectedCallback dcb;
      OnErrorCallback ecb;
      {
        std::lock_guard lk(cb_mutex_);
        dcb = on_disconnected_;
        ecb = on_error_;
      }
      if (ev.type == NetworkEventType::kError && ecb && ev.error) {
        ecb(*ev.error, ev.error_message);
      }
      if (dcb) {
        dcb();
      }
      break;
    }
    default:
      break;
    }
  }

  /* ================= Client API ================= */

  Client::Client() : impl_(std::make_unique<Impl>()) {}
  Client::~Client() = default;

  bool Client::ConnectIPv4(const std::string &h, uint16_t p)
  {
    return impl_->ConnectIPv4(h, p);
  }
  bool Client::ConnectIPv6(const std::string &h, uint16_t p)
  {
    return impl_->ConnectIPv6(h, p);
  }
  bool Client::ConnectUnixDomain(const std::string &p)
  {
    return impl_->ConnectUnixDomain(p);
  }
  bool Client::GracefulShutdown(int timeout_ms)
  {
    return impl_->GracefulShutdown(timeout_ms);
  }
  void Client::Disconnect() { impl_->Disconnect(); }
  bool Client::SendData(const uint8_t *d, size_t s)
  {
    return impl_->SendData(d, s);
  }
  bool Client::IsConnected() const { return impl_->IsConnected(); }
  void Client::SetOnConnected(OnConnectedCallback cb)
  {
    impl_->SetOnConnected(std::move(cb));
  }
  void Client::SetOnMessage(OnMessageCallback cb)
  {
    impl_->SetOnMessage(std::move(cb));
  }
  void Client::SetOnDisconnected(OnDisconnectedCallback cb)
  {
    impl_->SetOnDisconnected(std::move(cb));
  }
  void Client::SetOnError(OnErrorCallback cb)
  {
    impl_->SetOnError(std::move(cb));
  }

} // namespace darwincore::network
