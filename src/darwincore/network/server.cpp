//
// DarwinCore Network 模块
// 网络服务器实现
//
// 功能说明：
//   实现网络服务器的核心功能，包括启动监听、管理连接、发送数据等。
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <algorithm>
#include <csignal>
#include <mutex>
#include <unistd.h>

#include "acceptor.h"
#include "reactor.h"
#include "worker_pool.h"
#include <darwincore/network/configuration.h>
#include <darwincore/network/logger.h>
#include <darwincore/network/server.h>

namespace darwincore {
namespace network {

class Server::Impl {
public:
  Impl();
  ~Impl();

  bool StartIPv4(const std::string &host, uint16_t port);
  bool StartIPv6(const std::string &host, uint16_t port);
  bool StartUniversalIP(const std::string &host, uint16_t port);
  bool StartUnixDomain(const std::string &path);
  void Stop();

  bool SendData(uint64_t connection_id, const uint8_t *data, size_t size);

  void SetOnClientConnected(Server::OnClientConnectedCallback callback);
  void SetOnMessage(Server::OnMessageCallback callback);
  void SetOnClientDisconnected(Server::OnClientDisconnectedCallback callback);
  void SetOnConnectionError(Server::OnConnectionErrorCallback callback);

private:
  void OnNetworkEvent(const NetworkEvent &event);

  bool InitializeWorkerPool();
  bool InitializeReactors();

private:
  std::shared_ptr<WorkerPool> worker_pool_;
  std::vector<std::shared_ptr<Reactor>> reactors_;
  std::vector<std::unique_ptr<Acceptor>>
      acceptors_; ///< 多个 Acceptor 支持双栈监听
  std::unordered_map<uint64_t, size_t>
      conn_to_reactor_; ///< connection_id -> reactor_index

  Server::OnClientConnectedCallback on_client_connected_;
  Server::OnMessageCallback on_message_;
  Server::OnClientDisconnectedCallback on_client_disconnected_;
  Server::OnConnectionErrorCallback on_connection_error_;
};

Server::Impl::Impl() {
  // 全局忽略 SIGPIPE（只需设置一次）
  // 避免向已关闭的 socket 写入时进程崩溃
  static std::once_flag sigpipe_flag;
  std::call_once(sigpipe_flag, []() { signal(SIGPIPE, SIG_IGN); });
}

Server::Impl::~Impl() { Stop(); }

bool Server::Impl::InitializeWorkerPool() {
  if (worker_pool_ == nullptr) {
    worker_pool_ =
        std::make_shared<WorkerPool>(SocketConfiguration::kDefaultWorkerCount);
    if (!worker_pool_->Start()) {
      NW_LOG_ERROR("[Server::InitializeWorkerPool] WorkerPool 启动失败");
      return false;
    }
    NW_LOG_INFO("[Server] WorkerPool 已启动，worker_count="
                << SocketConfiguration::kDefaultWorkerCount);
  }
  return true;
}

bool Server::Impl::InitializeReactors() {
  if (!reactors_.empty()) {
    return true;
  }

  // Reactor 数量 = CPU 核数
  size_t reactor_count =
      std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

  for (size_t i = 0; i < reactor_count; ++i) {
    auto reactor = std::make_shared<Reactor>(i, worker_pool_);
    if (!reactor->Start()) {
      NW_LOG_ERROR("[Server::InitializeReactors] Reactor" << i << " 启动失败");
      return false;
    }
    reactors_.push_back(reactor);
  }

  NW_LOG_INFO("[Server] 创建了 " << reactor_count << " 个 Reactor");
  return true;
}

bool Server::Impl::StartIPv4(const std::string &host, uint16_t port) {
  if (!InitializeWorkerPool() || !InitializeReactors()) {
    NW_LOG_ERROR("[Server::StartIPv4] 初始化失败");
    return false;
  }

  auto acceptor = std::make_unique<Acceptor>();

  // 设置 Reactor 池（使用 weak_ptr，避免所有权循环）
  std::vector<std::weak_ptr<Reactor>> reactor_weak_ptrs;
  for (const auto &r : reactors_) {
    reactor_weak_ptrs.push_back(r);
  }
  acceptor->SetReactors(reactor_weak_ptrs);

  NW_LOG_INFO("[Server] WorkerPool 和 Reactor 已启动，设置事件回调...");
  // 设置事件回调
  worker_pool_->SetEventCallback([this](const NetworkEvent &event) {
    NW_LOG_DEBUG("[Server] 收到事件: type=" << static_cast<int>(event.type)
                                            << ", conn_id="
                                            << event.connection_id);
    OnNetworkEvent(event);
  });

  bool success = acceptor->ListenIPv4(host, port);
  if (success) {
    acceptors_.push_back(std::move(acceptor));
  }
  return success;
}

bool Server::Impl::StartIPv6(const std::string &host, uint16_t port) {
  if (!InitializeWorkerPool() || !InitializeReactors()) {
    NW_LOG_ERROR("[Server::StartIPv6] 初始化失败");
    return false;
  }

  auto acceptor = std::make_unique<Acceptor>();

  // 设置 Reactor 池（使用 weak_ptr，避免所有权循环）
  std::vector<std::weak_ptr<Reactor>> reactor_weak_ptrs;
  for (const auto &r : reactors_) {
    reactor_weak_ptrs.push_back(r);
  }
  acceptor->SetReactors(reactor_weak_ptrs);

  NW_LOG_INFO("[Server] WorkerPool 和 Reactor 已启动，设置事件回调...");
  // 设置事件回调
  worker_pool_->SetEventCallback([this](const NetworkEvent &event) {
    NW_LOG_DEBUG("[Server] 收到事件: type=" << static_cast<int>(event.type)
                                            << ", conn_id="
                                            << event.connection_id);
    OnNetworkEvent(event);
  });

  bool success = acceptor->ListenIPv6(host, port);
  if (success) {
    acceptors_.push_back(std::move(acceptor));
  }
  return success;
}

bool Server::Impl::StartUniversalIP(const std::string &host, uint16_t port) {
  // 双栈监听：先启动 IPv4，再启动 IPv6
  bool ipv4_success = StartIPv4(host, port);
  bool ipv6_success = StartIPv6(host, port);
  return ipv4_success && ipv6_success;
}

bool Server::Impl::StartUnixDomain(const std::string &path) {
  if (!InitializeWorkerPool() || !InitializeReactors()) {
    NW_LOG_ERROR("[Server::StartUnixDomain] 初始化失败");
    return false;
  }

  auto acceptor = std::make_unique<Acceptor>();

  // 设置 Reactor 池（使用 weak_ptr，避免所有权循环）
  std::vector<std::weak_ptr<Reactor>> reactor_weak_ptrs;
  for (const auto &r : reactors_) {
    reactor_weak_ptrs.push_back(r);
  }
  acceptor->SetReactors(reactor_weak_ptrs);

  NW_LOG_INFO("[Server] WorkerPool 和 Reactor 已启动，设置事件回调...");
  // 设置事件回调
  worker_pool_->SetEventCallback([this](const NetworkEvent &event) {
    NW_LOG_DEBUG("[Server] 收到事件: type=" << static_cast<int>(event.type)
                                            << ", conn_id="
                                            << event.connection_id);
    OnNetworkEvent(event);
  });

  bool success = acceptor->ListenUnixDomain(path);
  if (success) {
    acceptors_.push_back(std::move(acceptor));
  }
  return success;
}

void Server::Impl::Stop() {
  // 停止所有 Acceptor
  acceptors_.clear();
  reactors_.clear();
  worker_pool_.reset();
}

bool Server::Impl::SendData(uint64_t connection_id, const uint8_t *data,
                            size_t size) {
  if (reactors_.empty()) {
    NW_LOG_WARNING("[Server::SendData] reactors_.empty()，无法发送数据");
    return false;
  }
  // 从 connection_id 中提取 reactor_id
  // 新格式: | 24位: YYMMDD | 8位: reactor_id | 32位: seq |
  // reactor_id 在 bits 32-39
  size_t reactor_index = static_cast<size_t>((connection_id >> 32) & 0xFF);
  if (reactor_index >= reactors_.size()) {
    NW_LOG_ERROR("[Server::SendData] reactor_index="
                 << reactor_index << " 超出范围 (size=" << reactors_.size()
                 << ")");
    return false;
  }
  return reactors_[reactor_index]->SendData(connection_id, data, size);
}

void Server::Impl::SetOnClientConnected(
    Server::OnClientConnectedCallback callback) {
  on_client_connected_ = std::move(callback);
}

void Server::Impl::SetOnMessage(Server::OnMessageCallback callback) {
  on_message_ = std::move(callback);
}

void Server::Impl::SetOnClientDisconnected(
    Server::OnClientDisconnectedCallback callback) {
  on_client_disconnected_ = std::move(callback);
}

void Server::Impl::SetOnConnectionError(
    Server::OnConnectionErrorCallback callback) {
  on_connection_error_ = std::move(callback);
}

void Server::Impl::OnNetworkEvent(const NetworkEvent &event) {
  NW_LOG_TRACE("[Server::Impl::OnNetworkEvent] type="
               << static_cast<int>(event.type)
               << ", conn_id=" << event.connection_id
               << ", payload_size=" << event.payload.size());
  switch (event.type) {
  case NetworkEventType::kConnected:
    NW_LOG_TRACE("[Server] kConnected 事件");
    if (on_client_connected_ && event.connection_info) {
      NW_LOG_TRACE("[Server] 调用 on_client_connected_");
      on_client_connected_(*event.connection_info);
    }
    break;

  case NetworkEventType::kData:
    NW_LOG_TRACE(
        "[Server] kData 事件, on_message_=" << (on_message_ != nullptr));
    if (on_message_) {
      NW_LOG_TRACE("[Server] 调用 on_message_");
      on_message_(event.connection_id, event.payload);
      NW_LOG_TRACE("[Server] on_message_ 返回");
    }
    break;

  case NetworkEventType::kDisconnected:
    if (on_client_disconnected_) {
      on_client_disconnected_(event.connection_id);
    }
    break;

  case NetworkEventType::kError:
    if (on_connection_error_ && event.error) {
      on_connection_error_(event.connection_id, *event.error,
                           event.error_message);
    }
    break;
  }
}

Server::Server() : impl_(std::make_unique<Impl>()) {}

Server::~Server() = default;

bool Server::StartIPv4(const std::string &host, uint16_t port) {
  return impl_->StartIPv4(host, port);
}

bool Server::StartIPv6(const std::string &host, uint16_t port) {
  return impl_->StartIPv6(host, port);
}

bool Server::StartUniversalIP(const std::string &host, uint16_t port) {
  return impl_->StartUniversalIP(host, port);
}

bool Server::StartUnixDomain(const std::string &path) {
  return impl_->StartUnixDomain(path);
}

void Server::Stop() { impl_->Stop(); }

bool Server::SendData(uint64_t connection_id, const uint8_t *data,
                      size_t size) {
  return impl_->SendData(connection_id, data, size);
}

void Server::SetOnClientConnected(OnClientConnectedCallback callback) {
  impl_->SetOnClientConnected(std::move(callback));
}

void Server::SetOnMessage(OnMessageCallback callback) {
  impl_->SetOnMessage(std::move(callback));
}

void Server::SetOnClientDisconnected(OnClientDisconnectedCallback callback) {
  impl_->SetOnClientDisconnected(std::move(callback));
}

void Server::SetOnConnectionError(OnConnectionErrorCallback callback) {
  impl_->SetOnConnectionError(std::move(callback));
}

} // namespace network
} // namespace darwincore
