//
// DarwinCore Network 模块
// IOMonitor 实现
//
// 功能说明：
//   使用 kqueue (macOS/FreeBSD) 或 epoll (Linux) 实现 IO 事件监控。
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <cstring>
#include <unistd.h>

#include "io_monitor.h"
#include <darwincore/network/logger.h>

namespace darwincore {
namespace network {

IOMonitor::IOMonitor() : monitor_fd_(-1), timeout_ms_(1000) {
  NW_LOG_TRACE("[IOMonitor::IOMonitor] 构造函数");
}

IOMonitor::~IOMonitor() {
  NW_LOG_DEBUG("[IOMonitor::~IOMonitor] 析构函数");
  Close();
}

bool IOMonitor::Initialize() {
#if USE_KQUEUE
  NW_LOG_DEBUG("[IOMonitor::Initialize] 使用 kqueue");
  monitor_fd_ = kqueue();
#elif USE_EPOLL
  NW_LOG_DEBUG("[IOMonitor::Initialize] 使用 epoll");
  monitor_fd_ = epoll_create1(0);
#endif

  if (monitor_fd_ < 0) {
    NW_LOG_ERROR("[IOMonitor::Initialize] 创建监控器失败: " << strerror(errno));
    return false;
  }

  NW_LOG_INFO(
      "[IOMonitor::Initialize] IO 监控器初始化成功，fd=" << monitor_fd_);
  return true;
}

void IOMonitor::Close() {
  if (monitor_fd_ > 0) {
    NW_LOG_DEBUG("[IOMonitor::Close] 关闭监控器，fd=" << monitor_fd_);
    close(monitor_fd_);
    monitor_fd_ = -1;
  }
}

bool IOMonitor::StartReadMonitor(int fd) {
  NW_LOG_DEBUG("[IOMonitor::StartReadMonitor] 开始监控 fd=" << fd);

  if (monitor_fd_ == -1) {
    NW_LOG_ERROR("[IOMonitor::StartReadMonitor] 监控器未初始化！");
    return false;
  }

#if USE_KQUEUE
  struct kevent change;
  EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  int ret = kevent(monitor_fd_, &change, 1, nullptr, 0, nullptr);
#elif USE_EPOLL
  struct epoll_event event;
  event.events = EPOLLIN;
  event.data.fd = fd;
  int ret = epoll_ctl(monitor_fd_, EPOLL_CTL_ADD, fd, &event);
#endif

  if (ret < 0) {
    NW_LOG_ERROR("[IOMonitor::StartReadMonitor] 添加监控 fd="
                 << fd << " 失败: " << strerror(errno));
    return false;
  }

  NW_LOG_TRACE("[IOMonitor::StartReadMonitor] fd=" << fd << " 监控添加成功");
  return true;
}

bool IOMonitor::StartWriteMonitor(int fd) {
  NW_LOG_DEBUG("[IOMonitor::StartWriteMonitor] 开始监控写事件 fd=" << fd);

  if (monitor_fd_ == -1) {
    NW_LOG_ERROR("[IOMonitor::StartWriteMonitor] 监控器未初始化！");
    return false;
  }

#if USE_KQUEUE
  struct kevent change;
  EV_SET(&change, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
  int ret = kevent(monitor_fd_, &change, 1, nullptr, 0, nullptr);
#elif USE_EPOLL
  // epoll 需要修改已存在的 fd 的事件标志
  struct epoll_event event;
  event.events = EPOLLIN | EPOLLOUT; // 同时监控读和写
  event.data.fd = fd;
  int ret = epoll_ctl(monitor_fd_, EPOLL_CTL_MOD, fd, &event);
#endif

  if (ret < 0) {
    NW_LOG_ERROR("[IOMonitor::StartWriteMonitor] 添加写监控 fd="
                 << fd << " 失败: " << strerror(errno));
    return false;
  }

  NW_LOG_TRACE("[IOMonitor::StartWriteMonitor] fd=" << fd << " 写监控添加成功");
  return true;
}

bool IOMonitor::StopWriteMonitor(int fd) {
  NW_LOG_DEBUG("[IOMonitor::StopWriteMonitor] 停止写监控 fd=" << fd);

  if (monitor_fd_ <= 0) {
    NW_LOG_WARNING("[IOMonitor::StopWriteMonitor] 监控器未初始化");
    return false;
  }

#if USE_KQUEUE
  struct kevent change;
  EV_SET(&change, fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, 0, 0, nullptr);
  int ret = kevent(monitor_fd_, &change, 1, nullptr, 0, nullptr);
#elif USE_EPOLL
  // epoll 需要恢复为只监控读事件
  struct epoll_event event;
  event.events = EPOLLIN; // 只监控读
  event.data.fd = fd;
  int ret = epoll_ctl(monitor_fd_, EPOLL_CTL_MOD, fd, &event);
#endif

  if (ret < 0) {
    if (errno != ENOENT) {
      NW_LOG_WARNING("[IOMonitor::StopWriteMonitor] 停止写监控 fd="
                     << fd << " 失败: " << strerror(errno));
    }
    return false;
  }

  NW_LOG_TRACE("[IOMonitor::StopWriteMonitor] fd=" << fd << " 写监控停止成功");
  return true;
}

bool IOMonitor::StopMonitor(int fd) {
  NW_LOG_DEBUG("[IOMonitor::StopMonitor] 停止监控 fd=" << fd);

  if (monitor_fd_ <= 0) {
    NW_LOG_WARNING("[IOMonitor::StopMonitor] 监控器未初始化");
    return false;
  }

#if USE_KQUEUE
  struct kevent change;
  EV_SET(&change, fd, EVFILT_READ, EV_DELETE | EV_DISABLE, 0, 0, nullptr);
  int ret = kevent(monitor_fd_, &change, 1, nullptr, 0, nullptr);
#elif USE_EPOLL
  int ret = epoll_ctl(monitor_fd_, EPOLL_CTL_DEL, fd, nullptr);
#endif

  if (ret < 0) {
    // ENOENT 通常表示 fd 不在监控中，这种情况不应该记录为错误
    if (errno != ENOENT) {
      NW_LOG_WARNING("[IOMonitor::StopMonitor] 停止监控 fd="
                     << fd << " 失败: " << strerror(errno));
    }
    return false;
  }

  NW_LOG_TRACE("[IOMonitor::StopMonitor] fd=" << fd << " 监控停止成功");
  return true;
}

int IOMonitor::WaitEvents(void *events, int max_events, const int *timeout_ms) {
  if (events == nullptr || max_events == 0 || monitor_fd_ <= 0) {
    return 0;
  }

  struct timespec timeout {};
  struct timespec *timeout_ptr = nullptr;

  if (timeout_ms != nullptr) {
    timeout.tv_sec = *timeout_ms / 1000;
    timeout.tv_nsec = (*timeout_ms % 1000) * 1000 * 1000;
    timeout_ptr = &timeout;
  }

#if USE_KQUEUE
  int count =
      kevent(monitor_fd_, nullptr, 0, static_cast<struct kevent *>(events),
             max_events, timeout_ptr);
#elif USE_EPOLL
  int count = epoll_wait(monitor_fd_, static_cast<struct epoll_event *>(events),
                         max_events, *timeout_ms);
#endif

  // 如果有事件，记录日志（TRACE 级别，避免日志过多）
  if (count > 0) {
    NW_LOG_TRACE("[IOMonitor::WaitEvents] 收到 " << count << " 个事件");
  }

  return count;
}

} // namespace network
} // namespace darwincore
