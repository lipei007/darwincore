//
// DarwinCore Network 模块
// IOMonitor 实现（macOS kqueue）
//
// 功能说明：
//   使用 kqueue 实现 IO 事件监控。
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <cstring>
#include <unistd.h>

#include "io_monitor.h"
#include <darwincore/network/logger.h>

namespace darwincore
{
  namespace network
  {

    IOMonitor::IOMonitor() : kqueue_fd_(-1)
    {
      NW_LOG_TRACE("[IOMonitor::IOMonitor] 构造函数");
    }

    IOMonitor::~IOMonitor()
    {
      NW_LOG_DEBUG("[IOMonitor::~IOMonitor] 析构函数");
      Close();
    }

    bool IOMonitor::Initialize()
    {
      NW_LOG_DEBUG("[IOMonitor::Initialize] 使用 kqueue");
      kqueue_fd_ = kqueue();

      if (kqueue_fd_ < 0)
      {
        NW_LOG_ERROR("[IOMonitor::Initialize] 创建 kqueue 失败: " << strerror(errno));
        return false;
      }

      NW_LOG_INFO("[IOMonitor::Initialize] kqueue 初始化成功，fd=" << kqueue_fd_);
      return true;
    }

    void IOMonitor::Close()
    {
      if (kqueue_fd_ > 0)
      {
        NW_LOG_DEBUG("[IOMonitor::Close] 关闭 kqueue，fd=" << kqueue_fd_);
        close(kqueue_fd_);
        kqueue_fd_ = -1;
      }
    }

    bool IOMonitor::StartReadMonitor(int fd)
    {
      NW_LOG_DEBUG("[IOMonitor::StartReadMonitor] 开始监控读事件 fd=" << fd);

      if (kqueue_fd_ == -1)
      {
        NW_LOG_ERROR("[IOMonitor::StartReadMonitor] kqueue 未初始化！");
        return false;
      }

      struct kevent change;
      EV_SET(&change, fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
      int ret = kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr);

      if (ret < 0)
      {
        NW_LOG_ERROR("[IOMonitor::StartReadMonitor] 添加读监控 fd="
                     << fd << " 失败: " << strerror(errno));
        return false;
      }

      NW_LOG_TRACE("[IOMonitor::StartReadMonitor] fd=" << fd << " 读监控添加成功");
      return true;
    }

    bool IOMonitor::StopReadMonitor(int fd)
    {
      NW_LOG_DEBUG("[IOMonitor::StopReadMonitor] 停止读监控 fd=" << fd);

      if (kqueue_fd_ <= 0)
      {
        NW_LOG_WARNING("[IOMonitor::StopReadMonitor] kqueue 未初始化");
        return false;
      }

      struct kevent change;
      EV_SET(&change, fd, EVFILT_READ, EV_DELETE | EV_DISABLE, 0, 0, nullptr);
      int ret = kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr);

      if (ret < 0)
      {
        if (errno != ENOENT)
        {
          NW_LOG_WARNING("[IOMonitor::StopReadMonitor] 停止读监控 fd="
                         << fd << " 失败: " << strerror(errno));
        }
        return false;
      }

      NW_LOG_TRACE("[IOMonitor::StopReadMonitor] fd=" << fd << " 读监控停止成功");
      return true;
    }

    bool IOMonitor::StartWriteMonitor(int fd)
    {
      NW_LOG_DEBUG("[IOMonitor::StartWriteMonitor] 开始监控写事件 fd=" << fd);

      if (kqueue_fd_ == -1)
      {
        NW_LOG_ERROR("[IOMonitor::StartWriteMonitor] kqueue 未初始化！");
        return false;
      }

      struct kevent change;
      EV_SET(&change, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
      int ret = kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr);

      if (ret < 0)
      {
        NW_LOG_ERROR("[IOMonitor::StartWriteMonitor] 添加写监控 fd="
                     << fd << " 失败: " << strerror(errno));
        return false;
      }

      NW_LOG_TRACE("[IOMonitor::StartWriteMonitor] fd=" << fd << " 写监控添加成功");
      return true;
    }

    bool IOMonitor::StopWriteMonitor(int fd)
    {
      NW_LOG_DEBUG("[IOMonitor::StopWriteMonitor] 停止写监控 fd=" << fd);

      if (kqueue_fd_ <= 0)
      {
        NW_LOG_WARNING("[IOMonitor::StopWriteMonitor] kqueue 未初始化");
        return false;
      }

      struct kevent change;
      EV_SET(&change, fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, 0, 0, nullptr);
      int ret = kevent(kqueue_fd_, &change, 1, nullptr, 0, nullptr);

      if (ret < 0)
      {
        if (errno != ENOENT)
        {
          NW_LOG_WARNING("[IOMonitor::StopWriteMonitor] 停止写监控 fd="
                         << fd << " 失败: " << strerror(errno));
        }
        return false;
      }

      NW_LOG_TRACE("[IOMonitor::StopWriteMonitor] fd=" << fd << " 写监控停止成功");
      return true;
    }

    bool IOMonitor::StopMonitor(int fd)
    {
      NW_LOG_DEBUG("[IOMonitor::StopMonitor] 停止监控 fd=" << fd);

      if (kqueue_fd_ <= 0)
      {
        NW_LOG_WARNING("[IOMonitor::StopMonitor] kqueue 未初始化");
        return false;
      }

      struct kevent change[2];
      EV_SET(&change[0], fd, EVFILT_READ, EV_DELETE | EV_DISABLE, 0, 0, nullptr);
      EV_SET(&change[1], fd, EVFILT_WRITE, EV_DELETE | EV_DISABLE, 0, 0, nullptr);
      int ret = kevent(kqueue_fd_, change, 2, nullptr, 0, nullptr);

      if (ret < 0)
      {
        // ENOENT 通常表示 fd 不在监控中，这种情况不应该记录为错误
        if (errno != ENOENT)
        {
          NW_LOG_WARNING("[IOMonitor::StopMonitor] 停止监控 fd="
                         << fd << " 失败: " << strerror(errno));
        }
        return false;
      }

      NW_LOG_TRACE("[IOMonitor::StopMonitor] fd=" << fd << " 监控停止成功");
      return true;
    }

    int IOMonitor::WaitEvents(struct kevent *events, int max_events,
                              const int *timeout_ms)
    {
      if (events == nullptr || max_events == 0 || kqueue_fd_ <= 0)
      {
        return 0;
      }

      struct timespec timeout{};
      struct timespec *timeout_ptr = nullptr;

      if (timeout_ms != nullptr)
      {
        timeout.tv_sec = *timeout_ms / 1000;
        timeout.tv_nsec = (*timeout_ms % 1000) * 1000 * 1000;
        timeout_ptr = &timeout;
      }

      int count = kevent(kqueue_fd_, nullptr, 0, events, max_events, timeout_ptr);

      // 如果有事件，记录日志（TRACE 级别，避免日志过多）
      if (count > 0)
      {
        NW_LOG_TRACE("[IOMonitor::WaitEvents] 收到 " << count << " 个事件");
      }

      return count;
    }

  } // namespace network
} // namespace darwincore
