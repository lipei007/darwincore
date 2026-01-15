//
// DarwinCore Network 模块
// Reactor 实现
//
// 功能说明：
//   使用 macOS 上的 kqueue 实现 Reactor 事件循环。
//   管理文件描述符，处理 I/O 事件，并将事件转发给 WorkerPool 进行业务逻辑处理。
//   支持异步非阻塞发送和线程安全的操作队列。
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <arpa/inet.h>
#include <cstring>  // For memset, strerror
#include <ctime>    // For time, localtime
#include <errno.h>  // For errno, EAGAIN, etc.
#include <unistd.h> // For close, read, write

#include "io_monitor.h"
#include "reactor.h"
#include "socket_helper.h"
#include "worker_pool.h"
#include <darwincore/network/configuration.h>
#include <darwincore/network/logger.h>

namespace darwincore {
namespace network {

Reactor::Reactor(int id, const std::shared_ptr<WorkerPool> &worker_pool)
    : reactor_id_(id), worker_pool_(worker_pool),
      io_monitor_(std::make_unique<IOMonitor>()) {
  io_monitor_->Initialize();
}

Reactor::~Reactor() {
  Stop();
  // io_monitor_ 由 unique_ptr 自动释放
}

bool Reactor::Start() {
  if (is_running_.load()) {
    NW_LOG_WARNING("[Reactor" << reactor_id_ << "] 已经在运行中");
    return false;
  }

  if (!io_monitor_) {
    NW_LOG_ERROR("[Reactor" << reactor_id_ << "] io_monitor_ 为空");
    return false;
  }

  is_running_.store(true);
  event_loop_thread_ = std::thread(&Reactor::RunEventLoop, this);

  return true;
}

void Reactor::Stop() {
  if (!is_running_.load()) {
    return;
  }

  is_running_.store(false);
  pending_operations_.NotifyStop();

  if (event_loop_thread_.joinable()) {
    event_loop_thread_.join();
  }

  // 关闭所有连接的文件描述符
  NW_LOG_DEBUG("[Reactor" << reactor_id_ << "] 关闭 "
                          << fd_to_connection_id_.size() << " 个连接");
  for (auto &pair : fd_to_connection_id_) {
    int fd = pair.first;
    uint64_t connection_id = pair.second;
    NW_LOG_TRACE("[Reactor" << reactor_id_ << "] 关闭 fd=" << fd
                            << ", conn_id=" << connection_id);
    close(fd);
  }

  connections_.clear();
  fd_to_connection_id_.clear();
}

// ============ 公共方法（线程安全）============

uint64_t Reactor::AddConnection(int fd, const sockaddr_storage &peer) {
  // 如果 Reactor 未运行，直接返回失败
  if (!is_running_.load()) {
    NW_LOG_ERROR("[Reactor" << reactor_id_
                            << "::AddConnection] Reactor 未运行");
    return 0;
  }

  // 由于 AddConnection 需要同步返回 connection_id，这里直接执行
  // 注意：这个方法只能从 Acceptor 线程调用，而 Acceptor
  // 保证在连接分配时是单线程的
  return DoAddConnection(fd, peer);
}

bool Reactor::RemoveConnection(uint64_t connection_id) {
  if (!is_running_.load()) {
    return false;
  }

  Operation op;
  op.type = Operation::kRemove;
  op.connection_id = connection_id;
  pending_operations_.Enqueue(op);
  return true;
}

bool Reactor::SendData(uint64_t connection_id, const uint8_t *data,
                       size_t size) {
  NW_LOG_TRACE("[Reactor" << reactor_id_ << "::SendData] conn_id="
                          << connection_id << ", size=" << size);

  if (!is_running_.load()) {
    return false;
  }

  Operation op;
  op.type = Operation::kSend;
  op.connection_id = connection_id;
  op.data.assign(data, data + size);
  pending_operations_.Enqueue(op);
  return true;
}

void Reactor::SetEventCallback(EventCallback callback) {
  event_callback_ = std::move(callback);
}

// ============ 连接ID生成 ============

uint64_t Reactor::GenerateConnectionId() {
  // 获取当前日期 YYYYMMDD
  time_t now = time(nullptr);
  struct tm *tm_info = localtime(&now);
  int current_date = (1900 + tm_info->tm_year) * 10000 +
                     (1 + tm_info->tm_mon) * 100 + tm_info->tm_mday;

  // 如果日期变了，重置序号
  if (current_date != cached_date_) {
    cached_date_ = current_date;
    daily_sequence_.store(0);
  }

  // 递增序号
  uint32_t seq = daily_sequence_.fetch_add(1, std::memory_order_relaxed);

  // 跳过0（0表示无效ID）
  if (seq == 0) {
    seq = daily_sequence_.fetch_add(1, std::memory_order_relaxed);
  }

  // 格式：| 24位: YYYYMMDD去掉前2位 | 8位: reactor_id | 32位: 序号 |
  // 例如：2026-01-15 -> 260115, reactor_id=1, seq=1
  // = (260115 << 40) | (1 << 32) | 1
  uint32_t date_short = current_date % 1000000; // 取后6位 YYMMDD
  uint64_t connection_id = (static_cast<uint64_t>(date_short) << 40) |
                           (static_cast<uint64_t>(reactor_id_ & 0xFF) << 32) |
                           seq;

  return connection_id;
}

// ============ 私有方法（仅在 Reactor 线程执行）============

void Reactor::ProcessPendingOperations() {
  Operation op;
  while (pending_operations_.TryDequeue(op)) {
    switch (op.type) {
    case Operation::kAdd:
      DoAddConnection(op.fd, op.peer);
      break;
    case Operation::kRemove:
      DoRemoveConnection(op.connection_id);
      break;
    case Operation::kSend:
      DoSendData(op.connection_id, op.data);
      break;
    }
  }
}

uint64_t Reactor::DoAddConnection(int fd, const sockaddr_storage &peer) {
  NW_LOG_DEBUG("[Reactor" << reactor_id_ << "::DoAddConnection] fd=" << fd
                          << ", is_running=" << is_running_.load());
  if (fd < 0 || !is_running_.load()) {
    NW_LOG_ERROR("[Reactor" << reactor_id_ << "::DoAddConnection] 参数无效！");
    return 0;
  }

  // 生成日期+序号格式的 connection_id
  uint64_t connection_id = GenerateConnectionId();

  // 使用 IOMonitor 开始监控 fd
  if (!io_monitor_->StartReadMonitor(fd)) {
    NW_LOG_ERROR("[Reactor"
                 << reactor_id_
                 << "::DoAddConnection] 启动监控失败: " << strerror(errno));
    close(fd);
    return 0;
  }

  ReactorConnection conn(fd, peer, connection_id);
  connections_[connection_id] = conn;
  fd_to_connection_id_[fd] = connection_id;

  NW_LOG_DEBUG("[Reactor" << reactor_id_ << "::DoAddConnection] conn_id="
                          << connection_id << ", 提交 kConnected 事件");
  NetworkEvent connected_event(NetworkEventType::kConnected, connection_id);
  uint16_t port = 0;

  std::string ip = SocketHelper::AddressToString(peer, &port);
  bool is_unix_domain = SocketHelper::GetAddressFamily(peer) == AF_UNIX;

  connected_event.connection_info =
      ConnectionInformation(connection_id, ip, port, is_unix_domain);

  // 优先使用 WorkerPool 处理事件（Server 模式）
  if (worker_pool_) {
    worker_pool_->SubmitEvent(connected_event);
  } else if (event_callback_) {
    event_callback_(connected_event);
  }

  return connection_id;
}

bool Reactor::DoRemoveConnection(uint64_t connection_id) {
  auto it = connections_.find(connection_id);
  if (it == connections_.end()) {
    NW_LOG_WARNING("[Reactor" << reactor_id_
                              << "::DoRemoveConnection] conn_id 不存在: "
                              << connection_id);
    return false;
  }

  int fd = it->second.file_descriptor;

  // 停止监控 fd
  if (io_monitor_) {
    io_monitor_->StopMonitor(fd);
  }

  NW_LOG_TRACE("[Reactor" << reactor_id_ << "::DoRemoveConnection] 关闭 fd="
                          << fd << ", conn_id=" << connection_id);
  close(fd);

  fd_to_connection_id_.erase(fd);
  connections_.erase(it);

  return true;
}

bool Reactor::DoSendData(uint64_t connection_id,
                         const std::vector<uint8_t> &data) {
  NW_LOG_TRACE("[Reactor" << reactor_id_ << "::DoSendData] conn_id="
                          << connection_id << ", size=" << data.size());

  auto it = connections_.find(connection_id);
  if (it == connections_.end()) {
    NW_LOG_ERROR("[Reactor" << reactor_id_ << "::DoSendData] conn_id 不存在！");
    return false;
  }

  ReactorConnection &conn = it->second;
  int fd = conn.file_descriptor;

  // 如果已有待发送数据，追加到缓冲区
  if (!conn.send_buffer.empty()) {
    conn.send_buffer.insert(conn.send_buffer.end(), data.begin(), data.end());
    NW_LOG_TRACE("[Reactor" << reactor_id_
                            << "::DoSendData] 追加到缓冲区，总大小="
                            << conn.send_buffer.size());
    return true;
  }

  // 尝试直接发送
  const uint8_t *ptr = data.data();
  size_t remaining = data.size();
  size_t sent = 0;

  while (remaining > 0) {
    ssize_t ret = send(fd, ptr + sent, remaining, MSG_DONTWAIT);
    if (ret > 0) {
      sent += ret;
      remaining -= ret;
    } else if (ret < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // 缓冲剩余数据，注册可写事件
        conn.send_buffer.assign(ptr + sent, ptr + data.size());
        if (!conn.write_pending) {
          io_monitor_->StartWriteMonitor(fd);
          conn.write_pending = true;
        }
        NW_LOG_TRACE("[Reactor" << reactor_id_
                                << "::DoSendData] 部分发送，缓冲 "
                                << conn.send_buffer.size() << " 字节");
        return true;
      }
      // 其他错误
      NW_LOG_ERROR("[Reactor" << reactor_id_
                              << "::DoSendData] 发送失败: " << strerror(errno));
      HandleConnectionError(conn, errno);
      return false;
    }
  }

  NW_LOG_TRACE("[Reactor" << reactor_id_ << "::DoSendData] 发送成功: " << sent
                          << " 字节");
  return true;
}

void Reactor::RunEventLoop() {
  const int kEventBatchSize = SocketConfiguration::kDefaultEventBatchSize;

  NW_LOG_INFO("[Reactor" << reactor_id_ << "] 开始事件循环");
  while (is_running_.load()) {
    // 1. 先处理待执行的操作队列
    ProcessPendingOperations();

    if (!io_monitor_) {
      break;
    }

#if USE_KQUEUE
    struct kevent events[kEventBatchSize];
#else
    struct epoll_event events[kEventBatchSize];
#endif

    // 设置 100ms 超时，便于检查 is_running_ 和处理操作队列
    int timeout_ms = 100;
    int count = io_monitor_->WaitEvents(events, kEventBatchSize, &timeout_ms);

    if (count < 0) {
      if (errno == EINTR) {
        NW_LOG_TRACE("[Reactor" << reactor_id_
                                << "] WaitEvents 被信号中断 (EINTR)，重新等待");
        continue;
      }
      NW_LOG_ERROR("[Reactor" << reactor_id_ << "] WaitEvents 出错: "
                              << strerror(errno) << ", 退出事件循环");
      break;
    }

    if (count > 0) {
      NW_LOG_TRACE("[Reactor" << reactor_id_ << "] 收到 " << count
                              << " 个事件");
    }

    for (int i = 0; i < count; ++i) {
#if USE_KQUEUE
      int fd = events[i].ident;
      uint16_t flags = events[i].flags;
      int16_t filter = events[i].filter;
#else
      int fd = events[i].data.fd;
      uint32_t events_flags = events[i].events;
#endif

      NW_LOG_TRACE("[Reactor" << reactor_id_ << "] 事件: fd=" << fd);

      auto it = fd_to_connection_id_.find(fd);
      if (it == fd_to_connection_id_.end()) {
        NW_LOG_WARNING("[Reactor" << reactor_id_ << "] fd 不在映射中");
        continue;
      }

      auto conn_it = connections_.find(it->second);
      if (conn_it == connections_.end()) {
        continue;
      }

      ReactorConnection &conn = conn_it->second;

#if USE_KQUEUE
      if (flags & EV_EOF) {
        HandleConnectionClose(conn);
        continue;
      }

      if (filter == EVFILT_READ) {
        HandleReadEvent(fd);
      } else if (filter == EVFILT_WRITE) {
        HandleWriteEvent(fd);
      }
#else
      if (events_flags & EPOLLHUP) {
        HandleConnectionClose(conn);
        continue;
      }

      if (events_flags & EPOLLIN) {
        HandleReadEvent(fd);
      }
      if (events_flags & EPOLLOUT) {
        HandleWriteEvent(fd);
      }
#endif
    }
  }
  NW_LOG_INFO("[Reactor" << reactor_id_ << "] 事件循环结束");
}

void Reactor::HandleReadEvent(int fd) {
  NW_LOG_TRACE("[Reactor" << reactor_id_ << "::HandleReadEvent] fd=" << fd);
  uint8_t buffer[SocketConfiguration::kDefaultReceiveBufferSize];

  auto it = fd_to_connection_id_.find(fd);
  if (it == fd_to_connection_id_.end()) {
    NW_LOG_WARNING("[Reactor" << reactor_id_
                              << "::HandleReadEvent] fd 不在映射中！");
    return;
  }

  uint64_t connection_id = it->second;
  auto conn_it = connections_.find(connection_id);
  if (conn_it == connections_.end()) {
    NW_LOG_WARNING("[Reactor" << reactor_id_
                              << "::HandleReadEvent] conn_id 不在连接中！");
    return;
  }

  while (true) {
    ssize_t ret = recv(fd, buffer, sizeof(buffer), 0);

    if (ret > 0) {
      NW_LOG_TRACE("[Reactor" << reactor_id_ << "::HandleReadEvent] 收到 "
                              << ret << " 字节, conn_id=" << connection_id);
      NW_LOG_DEBUG("[Reactor" << reactor_id_ << "::HandleReadEvent] 收到 "
                              << ret << " 字节");
      NetworkEvent data_event(NetworkEventType::kData, connection_id);
      data_event.payload.assign(buffer, buffer + ret);

      if (worker_pool_) {
        NW_LOG_TRACE("[Reactor" << reactor_id_ << "] 提交事件到 WorkerPool");
        worker_pool_->SubmitEvent(data_event);
      } else if (event_callback_) {
        NW_LOG_TRACE("[Reactor" << reactor_id_ << "] 使用 event_callback_");
        event_callback_(data_event);
      }
    } else if (ret == 0) {
      NW_LOG_INFO("[Reactor" << reactor_id_
                             << "::HandleReadEvent] 连接关闭 (ret=0)");
      HandleConnectionClose(conn_it->second);
      return;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return;
      }
      NW_LOG_ERROR("[Reactor" << reactor_id_ << "::HandleReadEvent] 错误: "
                              << strerror(errno));
      HandleConnectionError(conn_it->second, errno);
      return;
    }
  }
}

void Reactor::HandleWriteEvent(int fd) {
  NW_LOG_TRACE("[Reactor" << reactor_id_ << "::HandleWriteEvent] fd=" << fd);

  auto it = fd_to_connection_id_.find(fd);
  if (it == fd_to_connection_id_.end()) {
    return;
  }

  auto conn_it = connections_.find(it->second);
  if (conn_it == connections_.end()) {
    return;
  }

  ReactorConnection &conn = conn_it->second;

  while (!conn.send_buffer.empty()) {
    ssize_t ret = send(fd, conn.send_buffer.data(), conn.send_buffer.size(),
                       MSG_DONTWAIT);
    if (ret > 0) {
      conn.send_buffer.erase(conn.send_buffer.begin(),
                             conn.send_buffer.begin() + ret);
      NW_LOG_TRACE("[Reactor" << reactor_id_ << "::HandleWriteEvent] 发送 "
                              << ret << " 字节，剩余 "
                              << conn.send_buffer.size());
    } else if (ret < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return; // 继续等待可写
      }
      NW_LOG_ERROR("[Reactor" << reactor_id_ << "::HandleWriteEvent] 发送失败: "
                              << strerror(errno));
      HandleConnectionError(conn, errno);
      return;
    }
  }

  // 发送完成，停止监控可写事件
  if (io_monitor_) {
    io_monitor_->StopWriteMonitor(fd);
  }
  conn.write_pending = false;
  NW_LOG_TRACE("[Reactor" << reactor_id_
                          << "::HandleWriteEvent] 缓冲区发送完成");
}

void Reactor::HandleConnectionClose(const ReactorConnection &conn) {
  uint64_t connection_id = conn.connection_id;

  NetworkEvent disconnected_event(NetworkEventType::kDisconnected,
                                  connection_id);

  if (worker_pool_) {
    worker_pool_->SubmitEvent(disconnected_event);
  } else if (event_callback_) {
    event_callback_(disconnected_event);
  }

  DoRemoveConnection(connection_id);
}

void Reactor::HandleConnectionError(const ReactorConnection &conn,
                                    int error_code) {
  uint64_t connection_id = conn.connection_id;

  NetworkEvent error_event(NetworkEventType::kError, connection_id);
  error_event.error = MapErrnoToNetworkError(error_code);
  error_event.error_message = strerror(error_code);

  if (worker_pool_) {
    worker_pool_->SubmitEvent(error_event);
  } else if (event_callback_) {
    event_callback_(error_event);
  }

  DoRemoveConnection(connection_id);
}

NetworkError Reactor::MapErrnoToNetworkError(int errno_val) {
  switch (errno_val) {
  case ECONNRESET:
    return NetworkError::kResetByPeer;
  case ETIMEDOUT:
    return NetworkError::kTimeout;
  case EPIPE:
    return NetworkError::kPeerClosed;
  default:
    return NetworkError::kSyscallFailure;
  }
}

} // namespace network
} // namespace darwincore
