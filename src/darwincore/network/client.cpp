//
// DarwinCore Network 模块
// 网络客户端实现
//
// 功能说明：
//   实现网络客户端的核心功能，包括连接服务器、发送数据、处理事件等。
//
// 作者: DarwinCore Network 团队
// 日期: 2026
//

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <csignal>
#include <mutex>

#include <darwincore/network/configuration.h>
#include <darwincore/network/logger.h>
#include "socket_helper.h"
#include "reactor.h"
#include "worker_pool.h"
#include <darwincore/network/client.h>

namespace darwincore {
namespace network {

class Client::Impl {
public:
  Impl();
  ~Impl();

  bool ConnectIPv4(const std::string& host, uint16_t port);
  bool ConnectIPv6(const std::string& host, uint16_t port);
  bool ConnectUnixDomain(const std::string& path);
  void Disconnect();

  bool SendData(const uint8_t* data, size_t size);
  bool IsConnected() const;

  void SetOnConnected(Client::OnConnectedCallback callback);
  void SetOnMessage(Client::OnMessageCallback callback);
  void SetOnDisconnected(Client::OnDisconnectedCallback callback);
  void SetOnError(Client::OnErrorCallback callback);

private:
  bool ConnectTo(int fd, const sockaddr* addr, socklen_t addr_len);

  void OnNetworkEvent(const NetworkEvent& event);

private:
  std::unique_ptr<WorkerPool> worker_pool_;
  std::unique_ptr<Reactor> reactor_;
  uint64_t connection_id_;
  std::atomic<bool> is_connected_;
  sockaddr_storage peer_address_;

  Client::OnConnectedCallback on_connected_;
  Client::OnMessageCallback on_message_;
  Client::OnDisconnectedCallback on_disconnected_;
  Client::OnErrorCallback on_error_;
};

Client::Impl::Impl() : connection_id_(0), is_connected_(false) {
  // 全局忽略 SIGPIPE（只需设置一次）
  // 避免向已关闭的 socket 写入时进程崩溃
  static std::once_flag sigpipe_flag;
  std::call_once(sigpipe_flag, []() {
    signal(SIGPIPE, SIG_IGN);
  });

  memset(&peer_address_, 0, sizeof(peer_address_));
  NW_LOG_TRACE("[Client::Impl] 构造函数完成");
}

Client::Impl::~Impl() {
  NW_LOG_DEBUG("[Client::~Impl] 析构函数，is_connected=" << is_connected_);
  Disconnect();
}

bool Client::Impl::ConnectIPv4(const std::string& host, uint16_t port) {
  NW_LOG_INFO("[Client::ConnectIPv4] 连接到 " << host << ":" << port);

  if (host.empty() || port == 0) {
    NW_LOG_ERROR("[Client::ConnectIPv4] 参数无效: host=" << host << ", port=" << port);
    return false;
  }

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    NW_LOG_ERROR("[Client::ConnectIPv4] 创建 socket 失败: " << strerror(errno));
    return false;
  }
  NW_LOG_DEBUG("[Client::ConnectIPv4] 创建 socket 成功，fd=" << fd);

  if (!SocketHelper::SetNonBlocking(fd)) {
    NW_LOG_ERROR("[Client::ConnectIPv4] 设置非阻塞失败: " << strerror(errno));
    close(fd);
    return false;
  }

  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);

  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
    NW_LOG_ERROR("[Client::ConnectIPv4] 地址解析失败: " << host);
    close(fd);
    return false;
  }

  bool success = ConnectTo(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (success) {
    NW_LOG_INFO("[Client::ConnectIPv4] 连接成功: " << host << ":" << port);
  } else {
    NW_LOG_ERROR("[Client::ConnectIPv4] 连接失败: " << host << ":" << port);
  }
  return success;
}

bool Client::Impl::ConnectIPv6(const std::string& host, uint16_t port) {
  NW_LOG_INFO("[Client::ConnectIPv6] 连接到 " << host << ":" << port);

  if (host.empty() || port == 0) {
    NW_LOG_ERROR("[Client::ConnectIPv6] 参数无效: host=" << host << ", port=" << port);
    return false;
  }

  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    NW_LOG_ERROR("[Client::ConnectIPv6] 创建 socket 失败: " << strerror(errno));
    return false;
  }
  NW_LOG_DEBUG("[Client::ConnectIPv6] 创建 socket 成功，fd=" << fd);

  if (!SocketHelper::SetNonBlocking(fd)) {
    NW_LOG_ERROR("[Client::ConnectIPv6] 设置非阻塞失败: " << strerror(errno));
    close(fd);
    return false;
  }

  sockaddr_in6 addr = {};
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(port);

  if (inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr) <= 0) {
    NW_LOG_ERROR("[Client::ConnectIPv6] 地址解析失败: " << host);
    close(fd);
    return false;
  }

  bool success = ConnectTo(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (success) {
    NW_LOG_INFO("[Client::ConnectIPv6] 连接成功: " << host << ":" << port);
  } else {
    NW_LOG_ERROR("[Client::ConnectIPv6] 连接失败: " << host << ":" << port);
  }
  return success;
}

bool Client::Impl::ConnectUnixDomain(const std::string& path) {
  NW_LOG_INFO("[Client::ConnectUnixDomain] 连接到 " << path);

  if (path.empty()) {
    NW_LOG_ERROR("[Client::ConnectUnixDomain] 路径为空");
    return false;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    NW_LOG_ERROR("[Client::ConnectUnixDomain] 创建 socket 失败: " << strerror(errno));
    return false;
  }
  NW_LOG_DEBUG("[Client::ConnectUnixDomain] 创建 socket 成功，fd=" << fd);

  if (!SocketHelper::SetNonBlocking(fd)) {
    NW_LOG_ERROR("[Client::ConnectUnixDomain] 设置非阻塞失败: " << strerror(errno));
    close(fd);
    return false;
  }

  sockaddr_un addr = {};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

  bool success = ConnectTo(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
  if (success) {
    NW_LOG_INFO("[Client::ConnectUnixDomain] 连接成功: " << path);
  } else {
    NW_LOG_ERROR("[Client::ConnectUnixDomain] 连接失败: " << path);
  }
  return success;
}

bool Client::Impl::ConnectTo(int fd, const sockaddr* addr,
                                     socklen_t addr_len) {
  NW_LOG_TRACE("[Client::ConnectTo] 开始连接，fd=" << fd);

  if (connect(fd, addr, addr_len) < 0 && errno != EINPROGRESS) {
    NW_LOG_ERROR("[Client::ConnectTo] connect 失败: " << strerror(errno));
    close(fd);
    return false;
  }
  NW_LOG_DEBUG("[Client::ConnectTo] connect 调用成功");

  memcpy(&peer_address_, addr, addr_len);

  // Client 创建一个单线程的 WorkerPool，避免阻塞 Reactor 线程
  // 这样业务逻辑在独立的 Worker 线程中处理，不会影响 Reactor 的 I/O 性能
  if (worker_pool_ == nullptr) {
    NW_LOG_DEBUG("[Client::ConnectTo] 创建 WorkerPool（单线程）");
    worker_pool_ = std::make_unique<WorkerPool>(1);  // Client 只需要一个 Worker
    if (!worker_pool_->Start()) {
      NW_LOG_ERROR("[Client::ConnectTo] WorkerPool 启动失败");
      close(fd);
      return false;
    }

    // 设置 WorkerPool 的回调，将事件转发给 OnNetworkEvent
    worker_pool_->SetEventCallback([this](const NetworkEvent& event) {
      OnNetworkEvent(event);
    });

    NW_LOG_INFO("[Client::ConnectTo] WorkerPool 启动成功");
  }

  if (reactor_ == nullptr) {
    NW_LOG_DEBUG("[Client::ConnectTo] 创建 Reactor");
    // Client 模式：传入 WorkerPool，事件将在 Worker 线程中处理
    reactor_ = std::make_unique<Reactor>(0, std::shared_ptr<WorkerPool>(worker_pool_.get(),
                                                                     [](WorkerPool*) {}));
    if (!reactor_->Start()) {
      NW_LOG_ERROR("[Client::ConnectTo] Reactor 启动失败");
      close(fd);
      return false;
    }
    NW_LOG_INFO("[Client::ConnectTo] Reactor 启动成功");
  }

  uint64_t conn_id = reactor_->AddConnection(fd, peer_address_);
  if (conn_id == 0) {
    NW_LOG_ERROR("[Client::ConnectTo] AddConnection 失败，返回 conn_id=0");
    return false;
  }

  connection_id_ = conn_id;
  NW_LOG_INFO("[Client::ConnectTo] 连接建立成功，conn_id=" << conn_id);
  is_connected_ = true;
  return true;
}

void Client::Impl::Disconnect() {
  NW_LOG_INFO("[Client::Disconnect] 断开连接，conn_id=" << connection_id_);
  is_connected_ = false;
  reactor_.reset();
  worker_pool_.reset();
  connection_id_ = 0;
  NW_LOG_DEBUG("[Client::Disconnect] 清理完成");
}

bool Client::Impl::SendData(const uint8_t* data, size_t size) {
  NW_LOG_TRACE("[Client::SendData] 发送数据，size=" << size << ", conn_id=" << connection_id_);

  if (!is_connected_ || connection_id_ == 0) {
    NW_LOG_WARNING("[Client::SendData] 客户端未连接，无法发送数据");
    return false;
  }

  bool success = reactor_->SendData(connection_id_, data, size);
  if (success) {
    NW_LOG_DEBUG("[Client::SendData] 发送成功，size=" << size);
  } else {
    NW_LOG_ERROR("[Client::SendData] 发送失败，conn_id=" << connection_id_);
  }
  return success;
}

bool Client::Impl::IsConnected() const {
  return is_connected_;
}

void Client::Impl::SetOnConnected(Client::OnConnectedCallback callback) {
  NW_LOG_TRACE("[Client::SetOnConnected] 设置连接回调");
  on_connected_ = std::move(callback);
}

void Client::Impl::SetOnMessage(Client::OnMessageCallback callback) {
  NW_LOG_TRACE("[Client::SetOnMessage] 设置消息回调");
  on_message_ = std::move(callback);
}

void Client::Impl::SetOnDisconnected(Client::OnDisconnectedCallback callback) {
  NW_LOG_TRACE("[Client::SetOnDisconnected] 设置断开连接回调");
  on_disconnected_ = std::move(callback);
}

void Client::Impl::SetOnError(Client::OnErrorCallback callback) {
  NW_LOG_TRACE("[Client::SetOnError] 设置错误回调");
  on_error_ = std::move(callback);
}

void Client::Impl::OnNetworkEvent(const NetworkEvent& event) {
  NW_LOG_TRACE("[Client::OnNetworkEvent] 收到事件: type=" << static_cast<int>(event.type)
            << ", conn_id=" << event.connection_id);

  connection_id_ = event.connection_id;

  switch (event.type) {
    case NetworkEventType::kConnected:
      NW_LOG_INFO("[Client::OnNetworkEvent] 连接成功");
      if (on_connected_ && event.connection_info) {
        NW_LOG_DEBUG("[Client::OnNetworkEvent] 调用用户连接回调");
        on_connected_(*event.connection_info);
      }
      is_connected_ = true;
      break;

    case NetworkEventType::kData:
      NW_LOG_DEBUG("[Client::OnNetworkEvent] 收到数据，size=" << event.payload.size());
      if (on_message_) {
        on_message_(event.payload);
      }
      break;

    case NetworkEventType::kDisconnected:
      NW_LOG_INFO("[Client::OnNetworkEvent] 连接断开");
      if (on_disconnected_) {
        on_disconnected_();
      }
      is_connected_ = false;
      break;

    case NetworkEventType::kError:
      NW_LOG_ERROR("[Client::OnNetworkEvent] 连接错误: " << event.error_message);
      if (on_error_ && event.error) {
        on_error_(*event.error, event.error_message);
      }
      is_connected_ = false;
      break;
  }
}

Client::Client() : impl_(std::make_unique<Impl>()) {
  NW_LOG_TRACE("[Client::Client] 构造完成");
}

Client::~Client() {
  NW_LOG_TRACE("[Client::~Client] 析构开始");
}

bool Client::ConnectIPv4(const std::string& host, uint16_t port) {
  return impl_->ConnectIPv4(host, port);
}

bool Client::ConnectIPv6(const std::string& host, uint16_t port) {
  return impl_->ConnectIPv6(host, port);
}

bool Client::ConnectUnixDomain(const std::string& path) {
  return impl_->ConnectUnixDomain(path);
}

void Client::Disconnect() {
  impl_->Disconnect();
}

bool Client::SendData(const uint8_t* data, size_t size) {
  return impl_->SendData(data, size);
}

bool Client::IsConnected() const {
  return impl_->IsConnected();
}

void Client::SetOnConnected(OnConnectedCallback callback) {
  impl_->SetOnConnected(std::move(callback));
}

void Client::SetOnMessage(OnMessageCallback callback) {
  impl_->SetOnMessage(std::move(callback));
}

void Client::SetOnDisconnected(OnDisconnectedCallback callback) {
  impl_->SetOnDisconnected(std::move(callback));
}

void Client::SetOnError(OnErrorCallback callback) {
  impl_->SetOnError(std::move(callback));
}

}
}
