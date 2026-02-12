//
// DarwinCore Network 模块
// ClientReactor 实现
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <algorithm>

#include "io_monitor.h"
#include "client_reactor.h"
#include "send_buffer.h"
#include "socket_helper.h"
#include "worker_pool.h"
#include "connection_id_generator.h"
#include <darwincore/network/configuration.h>
#include <darwincore/network/logger.h>

namespace darwincore
{
  namespace network
  {

    // ============ ClientReactor 实现 ============

    ClientReactor::ClientReactor(const std::shared_ptr<WorkerPool> &worker_pool)
        : worker_pool_(worker_pool),
          io_monitor_(std::make_unique<IOMonitor>())
    {
      if (!io_monitor_)
      {
        throw std::runtime_error("Failed to create IOMonitor");
      }
    }

    ClientReactor::~ClientReactor()
    {
      Stop();
    }

    bool ClientReactor::Start()
    {
      if (!io_monitor_->Initialize())
      {
        NW_LOG_ERROR("[ClientReactor] 初始化 IOMonitor 失败");
        return false;
      }
      if (!io_monitor_->InitializeWakeupEvent(kWakeupIdent))
      {
        NW_LOG_ERROR("[ClientReactor] 初始化唤醒事件失败");
        return false;
      }

      bool expected = false;
      if (!is_running_.compare_exchange_strong(expected, true))
      {
        NW_LOG_WARNING("[ClientReactor] 已经在运行中");
        return false;
      }

      try
      {
        event_loop_thread_ = std::thread(&ClientReactor::RunEventLoop, this);
        NW_LOG_INFO("[ClientReactor] 启动成功");
      }
      catch (const std::exception &e)
      {
        NW_LOG_ERROR("[ClientReactor] 启动失败: " << e.what());
        is_running_.store(false);
        return false;
      }

      return true;
    }

    void ClientReactor::Stop()
    {
      bool expected = true;
      if (!is_running_.compare_exchange_strong(expected, false))
      {
        return;
      }

      NW_LOG_INFO("[ClientReactor] 开始停止");

      send_operations_.NotifyStop();
      if (io_monitor_)
      {
        io_monitor_->TriggerWakeupEvent(kWakeupIdent);
      }

      // 立即关闭连接
      if (has_connection_.load() && connection_.file_descriptor >= 0)
      {
        shutdown(connection_.file_descriptor, SHUT_RDWR);
      }

      if (event_loop_thread_.joinable())
      {
        event_loop_thread_.join();
      }

      if (io_monitor_)
      {
        io_monitor_->Close();
      }

      if (has_connection_.load() && connection_.file_descriptor >= 0)
      {
        close(connection_.file_descriptor);
        connection_.file_descriptor = -1;
        has_connection_.store(false);
      }

      NW_LOG_INFO("[ClientReactor] 已停止");
    }

    bool ClientReactor::SetConnection(int fd, const sockaddr_storage &peer)
    {
      if (fd < 0)
      {
        NW_LOG_ERROR("[ClientReactor] 无效 fd");
        return false;
      }

      if (!is_running_.load(std::memory_order_acquire))
      {
        NW_LOG_ERROR("[ClientReactor] Reactor 未运行");
        return false;
      }

      // ClientReactor 只管理一个连接
      if (has_connection_.load())
      {
        NW_LOG_WARNING("[ClientReactor] 已有连接，先移除");
        RemoveConnection();
      }

      // 开始监控 fd
      if (!io_monitor_->StartReadMonitor(fd))
      {
        NW_LOG_ERROR("[ClientReactor] 启动监控失败: " << strerror(errno));
        close(fd);
        return false;
      }

      // 设置连接
      connection_.file_descriptor = fd;
      connection_.peer_address = peer;
      connection_.connection_id = ConnectionIdGenerator::Generate(255, static_cast<uint16_t>(fd), 1);
      connection_.UpdateActivity();

      has_connection_.store(true);

      NW_LOG_INFO("[ClientReactor] 设置连接: fd=" << fd << ", conn_id=" << connection_.connection_id);

      // 分发连接事件
      DispatchConnectedEvent();

      return true;
    }

    void ClientReactor::RemoveConnection()
    {
      if (!has_connection_.load())
      {
        return;
      }

      int fd = connection_.file_descriptor;

      // 停止监控
      if (io_monitor_)
      {
        io_monitor_->StopMonitor(fd);
      }

      close(fd);
      connection_.file_descriptor = -1;
      has_connection_.store(false);

      NW_LOG_INFO("[ClientReactor] 移除连接: fd=" << fd);

      // 分发断开事件
      DispatchDisconnectedEvent();
    }

    // ============ 发送数据实现 ============

    bool ClientReactor::SendAsync(const uint8_t *data, size_t size)
    {
      if (!data || size == 0)
      {
        return false;
      }

      if (!is_running_.load(std::memory_order_acquire))
      {
        return false;
      }

      SendOperation op;
      op.type = SendOperation::kAsync;
      op.data.assign(data, data + size);
      if (!send_operations_.Enqueue(op))
      {
        return false;
      }
      if (io_monitor_)
      {
        io_monitor_->TriggerWakeupEvent(kWakeupIdent);
      }
      return true;
    }

    bool ClientReactor::SendSync(const uint8_t *data, size_t size, int timeout_ms)
    {
      if (!data || size == 0)
      {
        return false;
      }

      if (!is_running_.load(std::memory_order_acquire))
      {
        return false;
      }

      auto promise = std::make_shared<std::promise<bool>>();
      auto future = promise->get_future();

      SendOperation op;
      op.type = SendOperation::kSync;
      op.data.assign(data, data + size);
      op.promise = promise;

      if (!send_operations_.Enqueue(op))
      {
        return false;
      }
      if (io_monitor_)
      {
        io_monitor_->TriggerWakeupEvent(kWakeupIdent);
      }

      // 等待发送完成
      if (timeout_ms > 0)
      {
        auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
        if (status != std::future_status::ready)
        {
          NW_LOG_WARNING("[ClientReactor] SendSync 超时");
          return false;
        }
      }
      else
      {
        future.wait();
      }

      try
      {
        return future.get();
      }
      catch (const std::exception &e)
      {
        NW_LOG_ERROR("[ClientReactor] SendSync 异常: " << e.what());
        return false;
      }
    }

    bool ClientReactor::SendAsyncWithCallback(const uint8_t *data, size_t size,
                                               SendCompleteCallback callback)
    {
      if (!data || size == 0)
      {
        return false;
      }

      if (!is_running_.load(std::memory_order_acquire))
      {
        return false;
      }

      SendOperation op;
      op.type = SendOperation::kAsyncCallback;
      op.data.assign(data, data + size);
      op.callback = std::move(callback);
      if (!send_operations_.Enqueue(op))
      {
        return false;
      }
      if (io_monitor_)
      {
        io_monitor_->TriggerWakeupEvent(kWakeupIdent);
      }
      return true;
    }

    // ============ 状态查询实现 ============

    size_t ClientReactor::GetSendBufferSize() const
    {
      if (!has_connection_.load())
      {
        return 0;
      }
      return connection_.send_buffer.Size();
    }

    bool ClientReactor::IsConnected() const
    {
      return has_connection_.load() && connection_.file_descriptor >= 0;
    }

    ClientReactor::Statistics ClientReactor::GetStatistics() const
    {
      Statistics stats;
      stats.total_bytes_sent = total_bytes_sent_.load(std::memory_order_relaxed);
      stats.total_bytes_received = total_bytes_received_.load(std::memory_order_relaxed);
      stats.pending_send_operations = send_operations_.Size();
      stats.send_buffer_size = GetSendBufferSize();
      return stats;
    }

    void ClientReactor::SetEventCallback(std::function<void(const NetworkEvent &)> callback)
    {
      event_callback_ = std::move(callback);
    }

    // ============ Reactor 线程私有方法 ============

    void ClientReactor::RunEventLoop()
    {
      pthread_setname_np("darwincore.client.reactor");
      const int kEventBatchSize = SocketConfiguration::kDefaultEventBatchSize;

      NW_LOG_INFO("[ClientReactor] 事件循环开始");

      while (is_running_.load(std::memory_order_acquire))
      {
        // 1. 处理待发送操作
        ProcessPendingOperations();

        // 2. 等待 I/O 事件
        struct kevent events[kEventBatchSize];
        int timeout_ms = 5000;
        int count = io_monitor_->WaitEvents(events, kEventBatchSize, &timeout_ms);

        if (count < 0)
        {
          if (errno == EINTR)
          {
            continue;
          }
          NW_LOG_ERROR("[ClientReactor] WaitEvents 失败: " << strerror(errno));
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
      }

      NW_LOG_INFO("[ClientReactor] 事件循环结束");
    }

    void ClientReactor::ProcessPendingOperations()
    {
      SendOperation op;
      const int kMaxBatchSize = 100;
      int processed = 0;

      while (processed < kMaxBatchSize && send_operations_.TryDequeue(op))
      {
        ++processed;

        bool success = DoSend(op.data.data(), op.data.size());

        // 处理回调
        if (op.callback)
        {
          size_t sent = success ? op.data.size() : 0;
          op.callback(success, sent);
        }

        // 处理同步发送的 promise
        if (op.promise)
        {
          op.promise->set_value(success);
        }
      }

      if (processed > 0)
      {
        // 尝试直接发送
        TrySendDirect();
      }
    }

    bool ClientReactor::DoSend(const uint8_t *data, size_t size)
    {
      if (!has_connection_.load())
      {
        NW_LOG_WARNING("[ClientReactor] 无连接");
        return false;
      }

      // 写入发送缓冲区
      if (!connection_.send_buffer.Write(data, size))
      {
        NW_LOG_ERROR("[ClientReactor] 写入发送缓冲区失败");
        return false;
      }

      // 尝试直接发送
      TrySendDirect();

      return true;
    }

    void ClientReactor::TrySendDirect()
    {
      if (!has_connection_.load())
      {
        return;
      }

      int fd = connection_.file_descriptor;
      if (fd < 0)
      {
        return;
      }

      // 尝试发送缓冲区数据
      ssize_t sent = connection_.send_buffer.SendToSocket(fd);

      if (sent > 0)
      {
        connection_.UpdateActivity();
        total_bytes_sent_.fetch_add(sent, std::memory_order_relaxed);
      }
      else if (sent < 0)
      {
        NW_LOG_ERROR("[ClientReactor] 发送失败: " << strerror(errno));
        RemoveConnection();
        return;
      }

      // 如果缓冲区非空，注册写事件
      if (!connection_.send_buffer.IsEmpty() && !connection_.write_pending)
      {
        io_monitor_->StartWriteMonitor(fd);
        connection_.write_pending = true;
      }

      // 发送完成，停止写监控
      if (connection_.send_buffer.IsEmpty() && connection_.write_pending)
      {
        io_monitor_->StopWriteMonitor(fd);
        connection_.write_pending = false;
      }
    }

    void ClientReactor::ProcessKqueueEvent(const struct kevent &event)
    {
      int fd = event.ident;
      uint16_t flags = event.flags;
      int16_t filter = event.filter;

      if (!has_connection_.load() || fd != connection_.file_descriptor)
      {
        return;
      }

      // 检查 EOF 和错误
      if (flags & EV_EOF)
      {
        NW_LOG_INFO("[ClientReactor] 对端关闭连接");
        RemoveConnection();
        return;
      }

      if (flags & EV_ERROR)
      {
        NW_LOG_ERROR("[ClientReactor] 连接错误: " << event.data);
        RemoveConnection();
        return;
      }

      // 处理读写事件
      if (filter == EVFILT_READ)
      {
        HandleReadEvent();
      }
      else if (filter == EVFILT_WRITE)
      {
        HandleWriteEvent();
      }
    }

    void ClientReactor::HandleReadEvent()
    {
      if (!has_connection_.load())
      {
        return;
      }

      int fd = connection_.file_descriptor;
      uint8_t buffer[SocketConfiguration::kDefaultReceiveBufferSize];

      while (true)
      {
        ssize_t ret = recv(fd, buffer, sizeof(buffer), 0);

        if (ret > 0)
        {
          connection_.UpdateActivity();
          total_bytes_received_.fetch_add(ret, std::memory_order_relaxed);

          // 分发数据事件
          DispatchDataEvent(buffer, ret);
        }
        else if (ret == 0)
        {
          // 对端关闭
          NW_LOG_INFO("[ClientReactor] 对端关闭连接");
          RemoveConnection();
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
            NW_LOG_ERROR("[ClientReactor] 接收错误: " << strerror(errno));
            RemoveConnection();
            return;
          }
        }
      }
    }

    void ClientReactor::HandleWriteEvent()
    {
      TrySendDirect();
    }

    // ============ 事件分发 ============

    void ClientReactor::DispatchEvent(const NetworkEvent &event)
    {
      if (worker_pool_)
      {
        worker_pool_->SubmitEvent(event);
      }
      else if (event_callback_)
      {
        event_callback_(event);
      }
    }

    void ClientReactor::DispatchDataEvent(const uint8_t *data, size_t size)
    {
      NetworkEvent event(NetworkEventType::kData, connection_.connection_id);
      event.payload.assign(data, data + size);
      DispatchEvent(event);
    }

    void ClientReactor::DispatchConnectedEvent()
    {
      NetworkEvent event(NetworkEventType::kConnected, connection_.connection_id);

      uint16_t port = 0;
      std::string ip = SocketHelper::AddressToString(connection_.peer_address, &port);
      bool is_unix = (SocketHelper::GetAddressFamily(connection_.peer_address) == AF_UNIX);

      event.connection_info = ConnectionInformation(connection_.connection_id, ip, port, is_unix);
      DispatchEvent(event);
    }

    void ClientReactor::DispatchDisconnectedEvent()
    {
      NetworkEvent event(NetworkEventType::kDisconnected, connection_.connection_id);
      DispatchEvent(event);
    }

  } // namespace network
} // namespace darwincore
