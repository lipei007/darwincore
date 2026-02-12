//
// DarwinCore Network 模块
// Reactor 实现（最终优化版）
//
// 主要改进：
//   1. 修复 AddConnection 同步/异步不一致问题
//   2. 重构 DoSendData，消除重复代码
//   3. 添加超时管理（时间轮）
//   4. 改进部分发送逻辑
//   5. 优化日志级别
//   6. 添加统计信息
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>

#include "io_monitor.h"
#include "reactor.h"
#include "send_buffer.h"
#include "socket_helper.h"
#include "worker_pool.h"
#include <darwincore/network/configuration.h>
#include <darwincore/network/logger.h>

namespace darwincore
{
  namespace network
  {

    // ============ ReactorConnection 增强 ============
    Reactor::ReactorConnection::ReactorConnection(int fd, const sockaddr_storage &peer, uint64_t conn_id)
        : file_descriptor(fd),
          peer_address(peer),
          connection_id(conn_id),
          last_active(std::chrono::steady_clock::now()) {}

    void Reactor::ReactorConnection::UpdateActivity()
    {
      last_active = std::chrono::steady_clock::now();
    }

    bool Reactor::ReactorConnection::IsTimeout(std::chrono::seconds timeout) const
    {
      auto now = std::chrono::steady_clock::now();
      return (now - last_active) > timeout;
    }

    // ============ Reactor 实现 ============

    Reactor::Reactor(int id, const std::shared_ptr<WorkerPool> &worker_pool)
        : reactor_id_(id),
          worker_pool_(worker_pool),
          io_monitor_(std::make_unique<IOMonitor>()),
          connection_timeout_(std::chrono::seconds(60))
    { // 默认 60 秒超时

      if (!io_monitor_)
      {
        throw std::runtime_error("Failed to create IOMonitor");
      }
      
    }

    Reactor::~Reactor()
    {
      Stop();
    }

    bool Reactor::Start()
    {

      if (!io_monitor_->Initialize()) {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] 初始化 IOMonitor 失败");
        return false;
      }
      if (!io_monitor_->InitializeWakeupEvent(kWakeupIdent)) {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] 初始化唤醒事件失败");
        return false;
      }

      bool expected = false;
      if (!is_running_.compare_exchange_strong(expected, true))
      {
        NW_LOG_WARNING("[Reactor" << reactor_id_ << "] 已经在运行中");
        return false;
      }

      try
      {
        event_loop_thread_ = std::thread(&Reactor::RunEventLoop, this);
        NW_LOG_INFO("[Reactor" << reactor_id_ << "] 启动成功");
      }
      catch (const std::exception &e)
      {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] 启动失败: " << e.what());
        is_running_.store(false);
        return false;
      }

      return true;
    }

    void Reactor::Stop()
    {
      bool expected = true;
      if (!is_running_.compare_exchange_strong(expected, false))
      {
        return;
      }

      NW_LOG_INFO("[Reactor" << reactor_id_ << "] 开始停止");

      // 先通知停止，这样事件循环会退出
      pending_operations_.NotifyStop();
      if (io_monitor_)
      {
        io_monitor_->TriggerWakeupEvent(kWakeupIdent);
      }

      if (event_loop_thread_.joinable())
      {
        event_loop_thread_.join();
      }

      // 先关闭 kqueue，移除所有监控
      if (io_monitor_) {
        io_monitor_->Close();
      }

      // 关闭所有连接
      CleanupAllConnections();

      NW_LOG_INFO("[Reactor" << reactor_id_ << "] 已停止");
    }

    // ============ 公共方法（线程安全）============

    bool Reactor::AddConnection(int fd, const sockaddr_storage &peer)
    {
      total_add_requests_.fetch_add(1, std::memory_order_relaxed);

      if (fd < 0)
      {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] AddConnection: 无效 fd");
        total_add_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }

      if (!is_running_.load(std::memory_order_acquire))
      {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] AddConnection: Reactor 未运行");
        total_add_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }

      Operation op;
      op.type = Operation::kAdd;
      op.fd = fd;
      op.peer = peer;

      if (!pending_operations_.Enqueue(op))
      {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] AddConnection: 队列已满");
        total_add_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      io_monitor_->TriggerWakeupEvent(kWakeupIdent);
      return true;
    }

    bool Reactor::RemoveConnection(uint64_t connection_id)
    {
      total_remove_requests_.fetch_add(1, std::memory_order_relaxed);

      if (!is_running_.load(std::memory_order_acquire))
      {
        total_remove_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }

      Operation op;
      op.type = Operation::kRemove;
      op.connection_id = connection_id;
      if (!pending_operations_.Enqueue(op))
      {
        total_remove_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      io_monitor_->TriggerWakeupEvent(kWakeupIdent);
      return true;
    }

    bool Reactor::SendData(uint64_t connection_id, const uint8_t *data, size_t size)
    {
      total_send_requests_.fetch_add(1, std::memory_order_relaxed);

      if (!data || size == 0)
      {
        total_send_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }

      if (!is_running_.load(std::memory_order_acquire))
      {
        total_send_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }

      Operation op;
      op.type = Operation::kSend;
      op.connection_id = connection_id;
      op.data.assign(data, data + size);
      if (!pending_operations_.Enqueue(op))
      {
        total_send_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      io_monitor_->TriggerWakeupEvent(kWakeupIdent);
      return true;
    }

    size_t Reactor::GetSendBufferSize(uint64_t connection_id)
    {
      if (!is_running_.load(std::memory_order_acquire))
      {
        return 0;
      }

      auto promise = std::make_shared<std::promise<uint64_t>>();
      auto future = promise->get_future();

      Operation op;
      op.type = Operation::kGetSendBufferSize;
      op.connection_id = connection_id;
      op.promise = promise;

      if (!pending_operations_.Enqueue(op))
      {
        return 0;
      }
      io_monitor_->TriggerWakeupEvent(kWakeupIdent);

      try
      {
        return static_cast<size_t>(future.get());
      }
      catch (const std::exception &)
      {
        return 0;
      }
    }

    void Reactor::SetEventCallback(EventCallback callback)
    {
      event_callback_ = std::move(callback);
    }

    void Reactor::SetConnectionTimeout(std::chrono::seconds timeout)
    {
      connection_timeout_ = timeout;
    }

    // ============ 私有方法（仅在 Reactor 线程执行）============

    void Reactor::ProcessPendingOperations()
    {
      Operation op;
      int processed = 0;
      const int kMaxBatchSize = 100;

      while (processed < kMaxBatchSize && pending_operations_.TryDequeue(op))
      {
        ++processed;

        switch (op.type)
        {
        case Operation::kAdd:
        {
          uint64_t conn_id = DoAddConnection(op.fd, op.peer);
          if (conn_id == 0)
          {
            total_add_failures_.fetch_add(1, std::memory_order_relaxed);
          }
          if (op.promise)
          {
            op.promise->set_value(conn_id);
          }
          break;
        }
        case Operation::kRemove:
          if (!DoRemoveConnection(op.connection_id))
          {
            total_remove_failures_.fetch_add(1, std::memory_order_relaxed);
          }
          break;
        case Operation::kSend:
          if (!DoSendData(op.connection_id, op.data))
          {
            total_send_failures_.fetch_add(1, std::memory_order_relaxed);
          }
          break;
        case Operation::kGetSendBufferSize:
        {
          uint64_t value = 0;
          auto it = connections_.find(op.connection_id);
          if (it != connections_.end())
          {
            value = static_cast<uint64_t>(it->second.send_buffer.Size());
          }
          if (op.promise)
          {
            op.promise->set_value(value);
          }
          break;
        }
        }
      }

      if (processed > 0)
      {
        total_ops_processed_.fetch_add(processed, std::memory_order_relaxed);
      }
    }

    uint64_t Reactor::DoAddConnection(int fd, const sockaddr_storage &peer)
    {
      if (fd < 0 || !is_running_.load())
      {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] DoAddConnection: 参数无效");
        return 0;
      }

      // 生成 connection_id
      uint16_t seq = connection_seq_.fetch_add(1, std::memory_order_relaxed);
      if (seq == 0)
      {
        seq = connection_seq_.fetch_add(1, std::memory_order_relaxed);
      }

      uint64_t connection_id = ConnectionIdGenerator::Generate(
          static_cast<uint8_t>(reactor_id_),
          static_cast<uint16_t>(fd),
          seq);

      // 检查连接是否已存在
      if (connections_.find(connection_id) != connections_.end())
      {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] connection_id 冲突: " << connection_id);
        close(fd);
        return 0;
      }

      // 开始监控 fd
      if (!io_monitor_->StartReadMonitor(fd))
      {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] 启动监控失败: " << strerror(errno));
        close(fd);
        return 0;
      }

      // 创建连接对象
      connections_.try_emplace(connection_id, fd, peer, connection_id);
      fd_to_connection_id_[fd] = connection_id;

      // 统计
      total_connections_.fetch_add(1, std::memory_order_relaxed);
      active_connections_.fetch_add(1, std::memory_order_relaxed);

      // 分发 kConnected 事件
      DispatchConnectionEvent(connection_id, peer);

      NW_LOG_INFO("[Reactor" << reactor_id_ << "] 添加连接: conn_id=" << connection_id
                             << ", fd=" << fd << ", active=" << active_connections_.load());

      return connection_id;
    }

    bool Reactor::DoRemoveConnection(uint64_t connection_id)
    {
      auto it = connections_.find(connection_id);
      if (it == connections_.end())
      {
        return false;
      }

      int fd = it->second.file_descriptor;

      // 停止监控
      if (io_monitor_)
      {
        io_monitor_->StopMonitor(fd);
      }

      close(fd);

      fd_to_connection_id_.erase(fd);
      connections_.erase(it);

      // 统计
      active_connections_.fetch_sub(1, std::memory_order_relaxed);

      NW_LOG_DEBUG("[Reactor" << reactor_id_ << "] 移除连接: conn_id=" << connection_id
                              << ", active=" << active_connections_.load());

      return true;
    }

    bool Reactor::DoSendData(uint64_t connection_id, const std::vector<uint8_t> &data)
    {
      auto it = connections_.find(connection_id);
      if (it == connections_.end())
      {
        NW_LOG_WARNING("[Reactor" << reactor_id_ << "] DoSendData: conn_id 不存在");
        return false;
      }

      ReactorConnection &conn = it->second;
      int fd = conn.file_descriptor;

      // 更新活跃时间(收到消息才算活跃)
      // conn.UpdateActivity();

      // 如果缓冲区非空，直接追加（避免乱序）
      if (!conn.send_buffer.IsEmpty())
      {
        return BufferAndMonitorWrite(conn, data.data(), data.size());
      }

      // 尝试直接发送
      size_t sent = 0;
      bool send_error = false;

      if (!TrySendDirect(fd, data.data(), data.size(), sent, send_error))
      {
        if (send_error)
        {
          HandleConnectionError(conn, errno);
          return false;
        }
        // EAGAIN，需要缓冲
      }

      // 如果有剩余数据，加入缓冲区
      if (sent < data.size())
      {
        return BufferAndMonitorWrite(conn, data.data() + sent, data.size() - sent);
      }

      // 统计
      total_bytes_sent_.fetch_add(sent, std::memory_order_relaxed);
      return true;
    }

    bool Reactor::TrySendDirect(int fd, const uint8_t *data, size_t size,
                                size_t &sent, bool &error)
    {
      sent = 0;
      error = false;

      ssize_t ret = send(fd, data, size, MSG_DONTWAIT | MSG_NOSIGNAL);

      if (ret > 0)
      {
        sent = ret;
        return ret == static_cast<ssize_t>(size); // 返回是否全部发送
      }
      else if (ret == 0)
      {
        // 不应该发生
        return false;
      }
      else
      {
        // ret < 0
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return false; // 缓冲区满，需要等待
        }
        else if (errno == EINTR)
        {
          return false; // 被信号中断，稍后重试
        }
        else
        {
          error = true; // 真正的错误
          return false;
        }
      }
    }

    bool Reactor::BufferAndMonitorWrite(ReactorConnection &conn,
                                        const uint8_t *data, size_t size)
    {
      int fd = conn.file_descriptor;

      // 写入缓冲区
      if (!conn.send_buffer.Write(data, size))
      {
        NW_LOG_ERROR("[Reactor" << reactor_id_ << "] 写入缓冲区失败");
        HandleConnectionError(conn, ENOMEM);
        return false;
      }

      // 检查高水位，触发背压
      if (conn.send_buffer.IsHighWaterMark() && !conn.read_paused)
      {
        io_monitor_->StopReadMonitor(fd);
        conn.read_paused = true;
        NW_LOG_WARNING("[Reactor" << reactor_id_ << "] 缓冲区高水位，暂停读取: fd="
                                  << fd << ", buffered=" << conn.send_buffer.Size());
      }

      // 注册写事件
      if (!conn.write_pending)
      {
        io_monitor_->StartWriteMonitor(fd);
        conn.write_pending = true;
      }

      return true;
    }

    void Reactor::RunEventLoop()
    {
      pthread_setname_np(("darwincore.network.reactor." + std::to_string(reactor_id_)).c_str());
      const int kEventBatchSize = SocketConfiguration::kDefaultEventBatchSize;
      auto last_timeout_check = std::chrono::steady_clock::now();

      NW_LOG_INFO("[Reactor" << reactor_id_ << "] 事件循环开始");

      while (is_running_.load(std::memory_order_acquire))
      {
        ProcessPendingOperations();

        // 1. 定期检查超时（每 5 秒）
        auto now = std::chrono::steady_clock::now();
        if (now - last_timeout_check >= std::chrono::seconds(5))
        {
          CheckTimeouts();
          last_timeout_check = now;
        }

        // 2. 等待 I/O 事件（通过 EVFILT_USER 可被跨线程即时唤醒）
        struct kevent events[kEventBatchSize];
        int timeout_ms = 5000;
        int count = io_monitor_->WaitEvents(events, kEventBatchSize, &timeout_ms);

        if (count < 0)
        {
          if (errno == EINTR)
          {
            continue;
          }
          NW_LOG_ERROR("[Reactor" << reactor_id_ << "] WaitEvents 失败: "
                                  << strerror(errno));
          break;
        }

        // 3. 处理 I/O 事件
        for (int i = 0; i < count; ++i)
        {
          if (events[i].filter == EVFILT_USER && events[i].ident == kWakeupIdent)
          {
            continue;
          }
          ProcessKqueueEvent(events[i]);
        }

        // 4. 处理待执行操作
        ProcessPendingOperations();
      }

      NW_LOG_INFO("[Reactor" << reactor_id_ << "] 事件循环结束");
    }

    void Reactor::ProcessKqueueEvent(const struct kevent &event)
    {
      int fd = event.ident;
      uint16_t flags = event.flags;
      int16_t filter = event.filter;

      auto it = fd_to_connection_id_.find(fd);
      if (it == fd_to_connection_id_.end())
      {
        return;
      }

      auto conn_it = connections_.find(it->second);
      if (conn_it == connections_.end())
      {
        return;
      }

      ReactorConnection &conn = conn_it->second;

      // 检查 EOF 和错误
      if (flags & EV_EOF)
      {
        HandleConnectionClose(conn);
        return;
      }

      if (flags & EV_ERROR)
      {
        HandleConnectionError(conn, event.data);
        return;
      }

      // 处理读写事件
      if (filter == EVFILT_READ)
      {
        HandleReadEvent(fd);
      }
      else if (filter == EVFILT_WRITE)
      {
        HandleWriteEvent(fd);
      }
    }

    void Reactor::HandleReadEvent(int fd)
    {
      auto it = fd_to_connection_id_.find(fd);
      if (it == fd_to_connection_id_.end())
      {
        return;
      }

      uint64_t connection_id = it->second;
      auto conn_it = connections_.find(connection_id);
      if (conn_it == connections_.end())
      {
        return;
      }

      ReactorConnection &conn = conn_it->second;
      uint8_t buffer[SocketConfiguration::kDefaultReceiveBufferSize];

      while (true)
      {
        ssize_t ret = recv(fd, buffer, sizeof(buffer), 0);

        if (ret > 0)
        {
          // 更新活跃时间
          conn.UpdateActivity();

          // 统计
          total_bytes_received_.fetch_add(ret, std::memory_order_relaxed);

          // 分发数据事件
          DispatchDataEvent(connection_id, buffer, ret);
        }
        else if (ret == 0)
        {
          // 对端关闭
          HandleConnectionClose(conn);
          return;
        }
        else
        {
          // ret < 0
          if (errno == EAGAIN || errno == EWOULDBLOCK)
          {
            return; // 数据读完
          }
          else if (errno == EINTR)
          {
            continue; // 被信号中断，重试
          }
          else
          {
            HandleConnectionError(conn, errno);
            return;
          }
        }
      }
    }

    void Reactor::HandleWriteEvent(int fd)
    {
      auto it = fd_to_connection_id_.find(fd);
      if (it == fd_to_connection_id_.end())
      {
        return;
      }

      auto conn_it = connections_.find(it->second);
      if (conn_it == connections_.end())
      {
        return;
      }

      ReactorConnection &conn = conn_it->second;

      // 发送缓冲区数据
      ssize_t sent = conn.send_buffer.SendToSocket(fd);

      if (sent > 0)
      {
        // 更新活跃时间
        conn.UpdateActivity();

        // 统计
        total_bytes_sent_.fetch_add(sent, std::memory_order_relaxed);

        // 检查低水位，恢复读取
        if (conn.read_paused && conn.send_buffer.IsLowWaterMark())
        {
          io_monitor_->StartReadMonitor(fd);
          conn.read_paused = false;
          NW_LOG_INFO("[Reactor" << reactor_id_ << "] 缓冲区低水位，恢复读取: fd=" << fd);
        }
      }
      else if (sent < 0)
      {
        HandleConnectionError(conn, errno);
        return;
      }

      // 发送完成，停止写监控
      if (conn.send_buffer.IsEmpty() && conn.write_pending)
      {
        io_monitor_->StopWriteMonitor(fd);
        conn.write_pending = false;
      }
    }

    void Reactor::CheckTimeouts()
    {
      if (connection_timeout_.count() == 0)
      {
        return; // 超时检查已禁用
      }

      std::vector<uint64_t> timeout_conns;

      for (auto &[conn_id, conn] : connections_)
      {
        if (conn.IsTimeout(connection_timeout_))
        {
          timeout_conns.push_back(conn_id);
        }
      }

      if (!timeout_conns.empty())
      {
        NW_LOG_WARNING("[Reactor" << reactor_id_ << "] 检测到 "
                                  << timeout_conns.size() << " 个超时连接");

        for (uint64_t conn_id : timeout_conns)
        {
          auto it = connections_.find(conn_id);
          if (it != connections_.end())
          {
            NW_LOG_INFO("[Reactor" << reactor_id_ << "] 关闭超时连接: " << conn_id);
            HandleConnectionClose(it->second);
          }
        }
      }
    }

    void Reactor::HandleConnectionClose(const ReactorConnection &conn)
    {
      uint64_t connection_id = conn.connection_id;

      DispatchDisconnectEvent(connection_id);
      DoRemoveConnection(connection_id);
    }

    void Reactor::HandleConnectionError(const ReactorConnection &conn, int error_code)
    {
      uint64_t connection_id = conn.connection_id;

      DispatchErrorEvent(connection_id, error_code);
      DispatchDisconnectEvent(connection_id);
      DoRemoveConnection(connection_id);
    }

    // ============ 事件分发 ============

    void Reactor::DispatchEvent(const NetworkEvent &event)
    {
      if (worker_pool_)
      {
        if (!worker_pool_->SubmitEvent(event))
        {
          total_dispatch_failures_.fetch_add(1, std::memory_order_relaxed);
        }
      }
      else if (event_callback_)
      {
        event_callback_(event);
      }
    }

    void Reactor::DispatchConnectionEvent(uint64_t connection_id,
                                          const sockaddr_storage &peer)
    {
      NetworkEvent event(NetworkEventType::kConnected, connection_id);

      uint16_t port = 0;
      std::string ip = SocketHelper::AddressToString(peer, &port);
      bool is_unix = (SocketHelper::GetAddressFamily(peer) == AF_UNIX);

      event.connection_info = ConnectionInformation(connection_id, ip, port, is_unix);
      DispatchEvent(event);
    }

    void Reactor::DispatchDataEvent(uint64_t connection_id,
                                    const uint8_t *data, size_t size)
    {
      NetworkEvent event(NetworkEventType::kData, connection_id);
      event.payload.assign(data, data + size);
      DispatchEvent(event);
    }

    void Reactor::DispatchDisconnectEvent(uint64_t connection_id)
    {
      NetworkEvent event(NetworkEventType::kDisconnected, connection_id);
      DispatchEvent(event);
    }

    void Reactor::DispatchErrorEvent(uint64_t connection_id, int error_code)
    {
      NetworkEvent event(NetworkEventType::kError, connection_id);
      event.error = MapErrnoToNetworkError(error_code);
      event.error_message = strerror(error_code);
      DispatchEvent(event);
    }

    // ============ 辅助方法 ============

    void Reactor::CleanupAllConnections()
    {
      NW_LOG_INFO("[Reactor" << reactor_id_ << "] 清理 "
                             << connections_.size() << " 个连接");

      for (auto &[conn_id, conn] : connections_)
      {
        int fd = conn.file_descriptor;
        if (io_monitor_)
        {
          io_monitor_->StopMonitor(fd);
        }
        close(fd);
      }

      connections_.clear();
      fd_to_connection_id_.clear();
    }

    NetworkError Reactor::MapErrnoToNetworkError(int errno_val)
    {
      switch (errno_val)
      {
      case ECONNRESET:
        return NetworkError::kResetByPeer;
      case ETIMEDOUT:
        return NetworkError::kTimeout;
      case EPIPE:
      case ENOTCONN:
        return NetworkError::kPeerClosed;
      case ECONNREFUSED:
        return NetworkError::kConnectionRefused;
      case EHOSTUNREACH:
      case ENETUNREACH:
        return NetworkError::kNetworkUnreachable;
      default:
        return NetworkError::kSyscallFailure;
      }
    }

    // ============ 统计接口 ============

    Reactor::Statistics Reactor::GetStatistics() const
    {
      Statistics stats;
      stats.reactor_id = reactor_id_;
      stats.total_connections = total_connections_.load(std::memory_order_relaxed);
      stats.active_connections = active_connections_.load(std::memory_order_relaxed);
      stats.total_bytes_sent = total_bytes_sent_.load(std::memory_order_relaxed);
      stats.total_bytes_received = total_bytes_received_.load(std::memory_order_relaxed);
      stats.total_ops_processed = total_ops_processed_.load(std::memory_order_relaxed);
      stats.total_dispatch_failures = total_dispatch_failures_.load(std::memory_order_relaxed);
      stats.total_add_requests = total_add_requests_.load(std::memory_order_relaxed);
      stats.total_add_failures = total_add_failures_.load(std::memory_order_relaxed);
      stats.total_remove_requests = total_remove_requests_.load(std::memory_order_relaxed);
      stats.total_remove_failures = total_remove_failures_.load(std::memory_order_relaxed);
      stats.total_send_requests = total_send_requests_.load(std::memory_order_relaxed);
      stats.total_send_failures = total_send_failures_.load(std::memory_order_relaxed);
      stats.pending_operation_queue_size = pending_operations_.Size();
      return stats;
    }

  } // namespace network
} // namespace darwincore
