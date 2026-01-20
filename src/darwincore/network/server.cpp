//
// DarwinCore Network 模块
// 网络服务器实现
//
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <algorithm>
#include <atomic>
#include <csignal>
#include <mutex>
#include <unistd.h>

#include "acceptor.h"
#include "connection_id_generator.h"
#include "reactor.h"
#include "worker_pool.h"
#include <darwincore/network/configuration.h>
#include <darwincore/network/logger.h>
#include <darwincore/network/server.h>

namespace darwincore
{
  namespace network
  {

    // ============ Server 状态枚举 ============
    enum class ServerState
    {
      kStopped,
      kStarting,
      kRunning,
      kStopping
    };

    // ============ Server::Impl 实现 ============
    class Server::Impl
    {
    public:
      Impl();
      ~Impl();

      // 启动接口
      bool StartIPv4(const std::string &host, uint16_t port);
      bool StartIPv6(const std::string &host, uint16_t port);
      bool StartUniversalIP(const std::string &host, uint16_t port);
      bool StartUnixDomain(const std::string &path);

      // 停止接口
      void Stop();
      bool IsRunning() const;

      // 数据发送
      bool SendData(uint64_t connection_id, const uint8_t *data, size_t size);

      // 回调设置
      void SetOnClientConnected(Server::OnClientConnectedCallback callback);
      void SetOnMessage(Server::OnMessageCallback callback);
      void SetOnClientDisconnected(Server::OnClientDisconnectedCallback callback);
      void SetOnConnectionError(Server::OnConnectionErrorCallback callback);

    private:
      // ============ 内部辅助方法 ============

      // 统一的启动流程
      template <typename StartFunc>
      bool StartInternal(StartFunc start_func, const char *type_name);

      // 初始化组件
      bool InitializeComponents();
      bool InitializeWorkerPool();
      bool InitializeReactors();

      // 创建 Acceptor 并启动
      bool CreateAndStartAcceptor(
          std::function<bool(Acceptor &)> listen_func,
          const char *description);

      // 事件处理
      void OnNetworkEvent(const NetworkEvent &event);

      // 状态转换
      bool TransitionState(ServerState expected, ServerState desired);
      ServerState GetState() const;

    private:
      // ============ 核心组件 ============
      std::shared_ptr<WorkerPool> worker_pool_;
      std::vector<std::shared_ptr<Reactor>> reactors_;
      std::vector<std::unique_ptr<Acceptor>> acceptors_;

      // ============ 回调函数 ============
      Server::OnClientConnectedCallback on_client_connected_;
      Server::OnMessageCallback on_message_;
      Server::OnClientDisconnectedCallback on_client_disconnected_;
      Server::OnConnectionErrorCallback on_connection_error_;

      // ============ 状态管理 ============
      std::atomic<ServerState> state_{ServerState::kStopped};
      mutable std::mutex state_mutex_; // 保护状态转换

      // ============ 统计信息 ============
      std::atomic<uint64_t> total_connections_{0};
      std::atomic<uint64_t> active_connections_{0};

      // ============ 配置 ============
      size_t reactor_count_{0};
    };

    // ============ 构造/析构 ============

    Server::Impl::Impl()
    {
      // 全局忽略 SIGPIPE（只需设置一次）
      static std::once_flag sigpipe_flag;
      std::call_once(sigpipe_flag, []()
                     {
                      signal(SIGPIPE, SIG_IGN);
                      NW_LOG_INFO("[Server] SIGPIPE 已忽略"); 
                    });
    }

    Server::Impl::~Impl()
    {
      Stop();
    }

    // ============ 状态管理 ============

    bool Server::Impl::TransitionState(ServerState expected, ServerState desired)
    {
      return state_.compare_exchange_strong(expected, desired);
    }

    ServerState Server::Impl::GetState() const
    {
      return state_.load(std::memory_order_acquire);
    }

    bool Server::Impl::IsRunning() const
    {
      return GetState() == ServerState::kRunning;
    }

    // ============ 初始化方法 ============

    bool Server::Impl::InitializeComponents()
    {
      std::lock_guard<std::mutex> lock(state_mutex_);

      if (!InitializeWorkerPool())
      {
        return false;
      }

      if (!InitializeReactors())
      {
        return false;
      }

      // 设置事件回调（只设置一次）
      worker_pool_->SetEventCallback([this](const NetworkEvent &event)
                                     {
                                      NW_LOG_TRACE("[Server] 收到事件: type=" 
                                        << static_cast<int>(event.type)    
                                        << ", conn_id=" << event.connection_id);
        
                                        OnNetworkEvent(event); 
                                      });

      return true;
    }

    bool Server::Impl::InitializeWorkerPool()
    {
      if (worker_pool_ != nullptr)
      {
        return true; // 已初始化
      }

      size_t worker_count = SocketConfiguration::kDefaultWorkerCount;

      worker_pool_ = std::make_shared<WorkerPool>(worker_count);

      if (!worker_pool_->Start())
      {
        NW_LOG_ERROR("[Server] WorkerPool 启动失败");
        worker_pool_.reset();
        return false;
      }

      NW_LOG_INFO("[Server] WorkerPool 已启动，worker_count=" << worker_count);
      return true;
    }

    bool Server::Impl::InitializeReactors()
    {
      if (!reactors_.empty())
      {
        return true; // 已初始化
      }

      // Reactor 数量 = CPU 核数（至少 1 个）
      reactor_count_ = std::max(1u, std::thread::hardware_concurrency());

      NW_LOG_INFO("[Server] 准备创建 " << reactor_count_ << " 个 Reactor");

      for (size_t i = 0; i < reactor_count_; ++i)
      {
        auto reactor = std::make_shared<Reactor>(i, worker_pool_);

        if (!reactor->Start())
        {
          NW_LOG_ERROR("[Server] Reactor " << i << " 启动失败");

          // 清理已创建的 Reactor
          for (auto &r : reactors_)
          {
            r->Stop();
          }
          reactors_.clear();
          return false;
        }

        reactors_.push_back(reactor);
        NW_LOG_DEBUG("[Server] Reactor " << i << " 启动成功");
      }

      NW_LOG_INFO("[Server] 所有 Reactor 已启动");
      return true;
    }

    // ============ Acceptor 创建 ============

    bool Server::Impl::CreateAndStartAcceptor(
        std::function<bool(Acceptor &)> listen_func,
        const char *description)
    {

      auto acceptor = std::make_unique<Acceptor>();

      // 设置 Reactor 池（使用 weak_ptr 避免循环引用）
      std::vector<std::weak_ptr<Reactor>> reactor_weak_ptrs;
      reactor_weak_ptrs.reserve(reactors_.size());

      for (const auto &r : reactors_)
      {
        reactor_weak_ptrs.push_back(r);
      }

      acceptor->SetReactors(reactor_weak_ptrs);

      // 执行监听
      if (!listen_func(*acceptor))
      {
        NW_LOG_ERROR("[Server] " << description << " 监听失败");
        return false;
      }

      NW_LOG_INFO("[Server] " << description << " 启动成功");
      acceptors_.push_back(std::move(acceptor));
      return true;
    }

    // ============ 启动方法（模板方法模式）============

    template <typename StartFunc>
    bool Server::Impl::StartInternal(StartFunc start_func, const char *type_name)
    {
      // 状态检查
      ServerState expected = ServerState::kStopped;
      if (!TransitionState(expected, ServerState::kStarting))
      {
        NW_LOG_WARNING("[Server] 服务器已在运行或正在启动/停止");
        return false;
      }

      NW_LOG_INFO("[Server] 开始启动 " << type_name << " 服务器");

      // 初始化组件
      if (!InitializeComponents())
      {
        NW_LOG_ERROR("[Server] 组件初始化失败");
        state_.store(ServerState::kStopped);
        return false;
      }

      // 执行具体的启动逻辑
      bool success = start_func();

      if (success)
      {
        state_.store(ServerState::kRunning);
        NW_LOG_INFO("[Server] " << type_name << " 服务器启动成功");
      }
      else
      {
        state_.store(ServerState::kStopped);
        NW_LOG_ERROR("[Server] " << type_name << " 服务器启动失败");
      }

      return success;
    }

    bool Server::Impl::StartIPv4(const std::string &host, uint16_t port)
    {
      return StartInternal([this, &host, port]()
                           { return CreateAndStartAcceptor(
                                 [&host, port](Acceptor &acceptor)
                                 {
                                   return acceptor.ListenIPv4(host, port);
                                 },
                                 "IPv4"); }, "IPv4");
    }

    bool Server::Impl::StartIPv6(const std::string &host, uint16_t port)
    {
      return StartInternal([this, &host, port]()
                           { return CreateAndStartAcceptor(
                                 [&host, port](Acceptor &acceptor)
                                 {
                                   return acceptor.ListenIPv6(host, port);
                                 },
                                 "IPv6"); }, "IPv6");
    }

    bool Server::Impl::StartUniversalIP(const std::string &host, uint16_t port)
    {
      return StartInternal([this, &host, port]()
                           {
    // 双栈监听：IPv4 + IPv6
    bool ipv4_ok = CreateAndStartAcceptor(
        [&host, port](Acceptor& acceptor) {
          return acceptor.ListenIPv4(host, port);
        },
        "IPv4 (双栈)");

    bool ipv6_ok = CreateAndStartAcceptor(
        [&host, port](Acceptor& acceptor) {
          return acceptor.ListenIPv6(host, port);
        },
        "IPv6 (双栈)");

    // 至少一个成功即可
    if (!ipv4_ok && !ipv6_ok) {
      NW_LOG_ERROR("[Server] IPv4 和 IPv6 都启动失败");
      return false;
    }

    if (!ipv4_ok) {
      NW_LOG_WARNING("[Server] IPv4 启动失败，仅使用 IPv6");
    }
    if (!ipv6_ok) {
      NW_LOG_WARNING("[Server] IPv6 启动失败，仅使用 IPv4");
    }

    return true; }, "双栈");
    }

    bool Server::Impl::StartUnixDomain(const std::string &path)
    {
      return StartInternal([this, &path]()
                           { return CreateAndStartAcceptor(
                                 [&path](Acceptor &acceptor)
                                 {
                                   return acceptor.ListenUnixDomain(path);
                                 },
                                 "Unix Domain Socket"); }, "Unix Domain");
    }

    // ============ 停止方法 ============

    void Server::Impl::Stop()
    {
      ServerState expected = ServerState::kRunning;
      if (!TransitionState(expected, ServerState::kStopping))
      {
        // 如果不是 Running 状态，检查是否已经是 Stopped
        if (GetState() == ServerState::kStopped)
        {
          return; // 已经停止
        }
        NW_LOG_WARNING("[Server] 服务器未在运行");
        return;
      }

      NW_LOG_INFO("[Server] 开始停止服务器");

      // 1. 停止接受新连接
      NW_LOG_DEBUG("[Server] 停止 Acceptor（" << acceptors_.size() << " 个）");
      acceptors_.clear();

      // 2. 停止所有 Reactor（会等待事件循环退出）
      NW_LOG_DEBUG("[Server] 停止 Reactor（" << reactors_.size() << " 个）");
      for (auto &reactor : reactors_)
      {
        if (reactor)
        {
          reactor->Stop();
        }
      }
      reactors_.clear();

      // 3. 停止 WorkerPool（会等待所有任务完成）
      NW_LOG_DEBUG("[Server] 停止 WorkerPool");
      if (worker_pool_)
      {
        worker_pool_->Stop();
        worker_pool_.reset();
      }

      // 4. 更新状态
      state_.store(ServerState::kStopped);

      NW_LOG_INFO("[Server] 服务器已停止，总连接数=" << total_connections_.load());
    }

    // ============ 数据发送 ============

    bool Server::Impl::SendData(uint64_t connection_id, const uint8_t *data,
                                size_t size)
    {
      if (!data || size == 0)
      {
        NW_LOG_WARNING("[Server::SendData] 无效参数");
        return false;
      }

      if (!IsRunning())
      {
        NW_LOG_WARNING("[Server::SendData] 服务器未运行");
        return false;
      }

      if (reactors_.empty())
      {
        NW_LOG_ERROR("[Server::SendData] reactors_ 为空");
        return false;
      }

      // 从 connection_id 中提取 reactor_id
      size_t reactor_id = ConnectionIdGenerator::GetReactorId(connection_id);

      if (reactor_id >= reactors_.size())
      {
        NW_LOG_ERROR("[Server::SendData] reactor_id=" << reactor_id
                                                      << " 超出范围（总数=" << reactors_.size() << "）");
        return false;
      }

      return reactors_[reactor_id]->SendData(connection_id, data, size);
    }

    // ============ 回调设置 ============

    void Server::Impl::SetOnClientConnected(
        Server::OnClientConnectedCallback callback)
    {
      on_client_connected_ = std::move(callback);
    }

    void Server::Impl::SetOnMessage(Server::OnMessageCallback callback)
    {
      on_message_ = std::move(callback);
    }

    void Server::Impl::SetOnClientDisconnected(
        Server::OnClientDisconnectedCallback callback)
    {
      on_client_disconnected_ = std::move(callback);
    }

    void Server::Impl::SetOnConnectionError(
        Server::OnConnectionErrorCallback callback)
    {
      on_connection_error_ = std::move(callback);
    }

    // ============ 事件处理 ============

    void Server::Impl::OnNetworkEvent(const NetworkEvent &event)
    {
      NW_LOG_TRACE("[Server::OnNetworkEvent] type=" << static_cast<int>(event.type)
                                                    << ", conn_id=" << event.connection_id
                                                    << ", payload_size=" << event.payload.size());

      switch (event.type)
      {
      case NetworkEventType::kConnected:
        total_connections_.fetch_add(1, std::memory_order_relaxed);
        active_connections_.fetch_add(1, std::memory_order_relaxed);

        NW_LOG_DEBUG("[Server] 新连接: conn_id=" << event.connection_id
                                                 << ", 活跃连接数=" << active_connections_.load());

        if (on_client_connected_ && event.connection_info)
        {
          try
          {
            on_client_connected_(*event.connection_info);
          }
          catch (const std::exception &e)
          {
            NW_LOG_ERROR("[Server] on_client_connected_ 异常: " << e.what());
          }
        }
        break;

      case NetworkEventType::kData:
        NW_LOG_TRACE("[Server] 收到数据: conn_id=" << event.connection_id
                                                   << ", size=" << event.payload.size());

        if (on_message_)
        {
          try
          {
            on_message_(event.connection_id, event.payload);
          }
          catch (const std::exception &e)
          {
            NW_LOG_ERROR("[Server] on_message_ 异常: " << e.what());
          }
        }
        break;

      case NetworkEventType::kDisconnected:
        active_connections_.fetch_sub(1, std::memory_order_relaxed);

        NW_LOG_DEBUG("[Server] 连接断开: conn_id=" << event.connection_id
                                                   << ", 活跃连接数=" << active_connections_.load());

        if (on_client_disconnected_)
        {
          try
          {
            on_client_disconnected_(event.connection_id);
          }
          catch (const std::exception &e)
          {
            NW_LOG_ERROR("[Server] on_client_disconnected_ 异常: " << e.what());
          }
        }
        break;

      case NetworkEventType::kError:
        NW_LOG_WARNING("[Server] 连接错误: conn_id=" << event.connection_id
                                                     << ", error=" << event.error_message);

        if (on_connection_error_ && event.error)
        {
          try
          {
            on_connection_error_(event.connection_id, *event.error,
                                 event.error_message);
          }
          catch (const std::exception &e)
          {
            NW_LOG_ERROR("[Server] on_connection_error_ 异常: " << e.what());
          }
        }
        break;

      default:
        NW_LOG_WARNING("[Server] 未知事件类型: " << static_cast<int>(event.type));
        break;
      }
    }

    // ============ Server 公共接口 ============

    Server::Server() : impl_(std::make_unique<Impl>()) {}

    Server::~Server() = default;

    bool Server::StartIPv4(const std::string &host, uint16_t port)
    {
      return impl_->StartIPv4(host, port);
    }

    bool Server::StartIPv6(const std::string &host, uint16_t port)
    {
      return impl_->StartIPv6(host, port);
    }

    bool Server::StartUniversalIP(const std::string &host, uint16_t port)
    {
      return impl_->StartUniversalIP(host, port);
    }

    bool Server::StartUnixDomain(const std::string &path)
    {
      return impl_->StartUnixDomain(path);
    }

    void Server::Stop()
    {
      impl_->Stop();
    }

    bool Server::SendData(uint64_t connection_id, const uint8_t *data, size_t size)
    {
      return impl_->SendData(connection_id, data, size);
    }

    void Server::SetOnClientConnected(OnClientConnectedCallback callback)
    {
      impl_->SetOnClientConnected(std::move(callback));
    }

    void Server::SetOnMessage(OnMessageCallback callback)
    {
      impl_->SetOnMessage(std::move(callback));
    }

    void Server::SetOnClientDisconnected(OnClientDisconnectedCallback callback)
    {
      impl_->SetOnClientDisconnected(std::move(callback));
    }

    void Server::SetOnConnectionError(OnConnectionErrorCallback callback)
    {
      impl_->SetOnConnectionError(std::move(callback));
    }

  } // namespace network
} // namespace darwincore