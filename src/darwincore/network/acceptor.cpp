//
// DarwinCore Network 模块
// Acceptor 实现
//
// 功能说明：
//   Acceptor 负责监听 Socket 并接受新的客户端连接。
//   接受新连接后，将 fd 分配给 Reactor 线程池。
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "acceptor.h"
#include "io_monitor.h"
#include "reactor.h"
#include "socket_helper.h"
#include <darwincore/network/configuration.h>
#include <darwincore/network/logger.h>
#include <pthread.h>

// SOMAXCONN 可能在某些系统上未定义
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

namespace darwincore
{
  namespace network
  {
    Acceptor::Acceptor()
        : listen_fd_(-1), io_monitor_(std::make_unique<IOMonitor>()),
          next_reactor_index_(0), is_running_(false)
    {
      
    }

    Acceptor::~Acceptor()
    {
      Stop();
      // io_monitor_ 由 unique_ptr 自动释放
    }

    bool Acceptor::ListenIPv4(const std::string &host, uint16_t port)
    {
      return ListenGeneric(SocketProtocol::kIPv4, host, port);
    }

    bool Acceptor::ListenIPv6(const std::string &host, uint16_t port)
    {
      return ListenGeneric(SocketProtocol::kIPv6, host, port);
    }

    bool Acceptor::ListenUnixDomain(const std::string &path)
    {
      bool result = ListenGeneric(SocketProtocol::kUnixDomain, path, 0);
      if (result)
      {
        unix_socket_path_ = path;
      }
      return result;
    }

    void Acceptor::SetReactors(
        const std::vector<std::weak_ptr<Reactor>> &reactors)
    {
      // 直接存储 weak_ptr，不延长生命周期
      reactors_ = reactors;
    }

    void Acceptor::Stop()
    {
      // 使用 compare_exchange 确保只执行一次停止逻辑
      bool expected = true;
      if (!is_running_.compare_exchange_strong(expected, false))
      {
        // 已经停止或正在停止
        return;
      }

      NW_LOG_DEBUG("[Acceptor::Stop] 开始停止 Acceptor");

      // 唤醒 accept 线程
      if (io_monitor_)
      {
        // 通过停止监控器来唤醒 kevent/epoll_wait
        if (listen_fd_ >= 0)
        {
          io_monitor_->StopMonitor(listen_fd_);
        }
      }

      if (accept_thread_.joinable())
      {
        accept_thread_.join();
        NW_LOG_DEBUG("[Acceptor::Stop] accept_thread 已结束");
      }

      if (listen_fd_ >= 0)
      {
        close(listen_fd_);
        listen_fd_ = -1;
      }

      // 清理 Unix Domain Socket 文件
      if (!unix_socket_path_.empty())
      {
        SocketHelper::UnlinkUnixDomainSocket(unix_socket_path_);
        unix_socket_path_.clear();
        NW_LOG_DEBUG("[Acceptor::Stop] 已清理 Unix Domain Socket: "
                     << unix_socket_path_);
      }

      NW_LOG_INFO("[Acceptor::Stop] Acceptor 已完全停止");
    }

    bool Acceptor::IsRunning() const
    {
      return is_running_.load();
    }

    bool Acceptor::ListenGeneric(SocketProtocol protocol, const std::string &host,
                                 uint16_t port)
    {

      if (!io_monitor_->Initialize())
      {
        NW_LOG_ERROR("[Acceptor::ListenGeneric] io_monitor_ 初始化失败");
        return false
      }
      

      // 创建 Socket
      int fd = SocketHelper::CreateSocket(protocol);
      if (fd < 0)
      {
        NW_LOG_ERROR("[Acceptor::ListenGeneric] CreateSocket 失败");
        return false;
      }

      // 设置非阻塞
      if (!SocketHelper::SetNonBlocking(fd))
      {
        NW_LOG_ERROR("[Acceptor::ListenGeneric] SetNonBlocking 失败");
        close(fd);
        return false;
      }

      // 设置 SO_REUSEADDR (允许端口快速重用)
      int opt = 1;
      if (!SocketHelper::SetSocketOption(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)))
      {
        NW_LOG_WARNING("[Acceptor::ListenGeneric] SO_REUSEADDR 设置失败");
        close(fd);
        return false;
      }

      // 设置 SO_REUSEPORT (允许多个 socket 监听同一端口)
      // 这对于负载均衡和多进程很有用
      bool reuseport_set = SocketHelper::SetSocketOption(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
      if (!reuseport_set)
      {
        // 忽略失败，SO_REUSEPORT 不是所有平台都支持
        NW_LOG_DEBUG("[Acceptor::ListenGeneric] SO_REUSEPORT 设置失败（可能不支持）");
      }

      // 解析并绑定地址
      sockaddr_storage addr = {};
      if (!SocketHelper::ResolveAddress(host, port, protocol, &addr))
      {
        NW_LOG_ERROR("[Acceptor::ListenGeneric] ResolveAddress 失败: " << host);
        close(fd);
        return false;
      }

      // 对于 Unix Domain Socket，在 bind 之前清理旧文件
      if (protocol == SocketProtocol::kUnixDomain)
      {
        SocketHelper::UnlinkUnixDomainSocket(host);
      }

      // 计算正确的地址长度
      socklen_t addr_len = 0;
      switch (addr.ss_family)
      {
      case AF_INET:
        addr_len = sizeof(sockaddr_in);
        break;
      case AF_INET6:
        addr_len = sizeof(sockaddr_in6);
        break;
      case AF_UNIX:
        addr_len = sizeof(sockaddr_un);
        break;
      default:
        addr_len = sizeof(addr);
        break;
      }

      if (bind(fd, reinterpret_cast<const sockaddr *>(&addr), addr_len) < 0)
      {
        NW_LOG_ERROR("[Acceptor::ListenGeneric] bind 失败: " << strerror(errno)
                                                             << ", fd=" << fd);
        close(fd);
        return false;
      }

      // 开始监听
      // 使用系统默认的 backlog (SOMAXCONN)
      // 这已经是平台优化的值，通常在 128-4096 之间
      if (listen(fd, SOMAXCONN) < 0)
      {
        NW_LOG_ERROR("[Acceptor::ListenGeneric] listen 失败: " << strerror(errno)
                                                               << ", fd=" << fd);
        close(fd);
        return false;
      }

      protocol_ = protocol;
      listen_fd_ = fd;
      NW_LOG_INFO("[Acceptor::ListenGeneric] 监听成功: fd="
                  << fd << ", backlog=" << SOMAXCONN << ", SO_REUSEADDR=1"
                  << ", SO_REUSEPORT=" << (reuseport_set ? "1" : "0")
                  << ", protocol=" << static_cast<int>(protocol));

      is_running_.store(true);
      accept_thread_ = std::thread(&Acceptor::AcceptLoop, this);

      return true;
    }

    void Acceptor::AcceptLoop()
    {
      pthread_setname_np(("darwincore.network.acceptor" + std::to_string(protocol_)).c_str());
      NW_LOG_DEBUG("[Acceptor::AcceptLoop] AcceptLoop 启动，listen_fd=" << listen_fd_);

      if (!io_monitor_)
      {
        NW_LOG_ERROR("[Acceptor::AcceptLoop] io_monitor_ 为空");
        return;
      }

      // 监听 listen_fd 的可读事件
      if (!io_monitor_->StartReadMonitor(listen_fd_))
      {
        NW_LOG_ERROR("[Acceptor::AcceptLoop] 启动监控 listen_fd 失败");
        return;
      }

      NW_LOG_INFO("[Acceptor::AcceptLoop] 开始监听连接...");

      // Acceptor 只需要监听一个 fd，不需要太多事件槽位
      const int kMaxEvents = 2;
      struct kevent events[kMaxEvents];

      while (is_running_.load())
      {
        // 使用较长的超时时间减少不必要的 CPU 唤醒
        // 1 秒足够响应停止信号
        int timeout_ms = 1000;
        int nev = io_monitor_->WaitEvents(events, kMaxEvents, &timeout_ms);

        if (nev < 0)
        {
          if (errno == EINTR)
          {
            NW_LOG_TRACE("[Acceptor::AcceptLoop] 被信号中断，继续等待");
            continue; // 被信号中断
          }
          NW_LOG_ERROR("[Acceptor::AcceptLoop] WaitEvents 出错: " << strerror(errno));
          // 记录错误但继续运行，除非是严重错误
          if (errno == EBADF || errno == EFAULT)
          {
            NW_LOG_ERROR("[Acceptor::AcceptLoop] 严重错误，退出循环");
            break;
          }
          continue;
        }

        // 超时或没有事件
        if (nev == 0)
        {
          continue;
        }

        for (int i = 0; i < nev; ++i)
        {
          int fd = static_cast<int>(events[i].ident);

          if (fd == listen_fd_)
          {
            // 批量接受连接，提高性能
            // 基于时间片，在一次事件触发中尽可能多地接受连接
            auto start = std::chrono::steady_clock::now();
            while(is_running_.load())
            {
              sockaddr_storage peer_addr = {};
              socklen_t peer_len = sizeof(peer_addr);

              int client_fd = accept(listen_fd_, reinterpret_cast<sockaddr *>(&peer_addr), &peer_len);

              if (client_fd < 0)
              {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                  // 没有更多连接了，跳出内层循环
                  break;
                }
                // 记录错误但继续处理其他连接
                NW_LOG_ERROR(
                    "[Acceptor::AcceptLoop] accept 失败: " << strerror(errno));
                continue;
              }

              NW_LOG_TRACE(
                  "[Acceptor::AcceptLoop] 接受新连接，client_fd=" << client_fd);

              // 设置客户端 Socket 为非阻塞
              if (!SocketHelper::SetNonBlocking(client_fd))
              {
                NW_LOG_ERROR("[Acceptor::AcceptLoop] 设置非阻塞失败，关闭 client_fd="
                             << client_fd);
                close(client_fd);
                continue;
              }

              // 设置 TCP_NODELAY，禁用 Nagle 算法，降低延迟
              int flag = 1;
              SocketHelper::SetSocketOption(client_fd, IPPROTO_TCP, TCP_NODELAY,
                                            &flag, sizeof(flag));

              // 设置发送/接收缓冲区大小 (64KB)
              int buf_size = 64 * 1024;
              SocketHelper::SetSocketOption(client_fd, SOL_SOCKET, SO_SNDBUF,
                                            &buf_size, sizeof(buf_size));
              SocketHelper::SetSocketOption(client_fd, SOL_SOCKET, SO_RCVBUF,
                                            &buf_size, sizeof(buf_size));

              NW_LOG_DEBUG("[Acceptor::AcceptLoop] 新连接 fd="
                           << client_fd << " 准备分配给 Reactor");

              // 将新连接分配给 Reactor
              AssignToReactor(client_fd, peer_addr);

              auto end = std::chrono::steady_clock::now();
              if (end - start > 200us)
              {
                break;
              }
              

            }
          }
        }
      }

      // 停止监听 listen_fd（可能已经在 Stop() 中调用）
      NW_LOG_DEBUG("[Acceptor::AcceptLoop] AcceptLoop 结束，停止监听 listen_fd="
                   << listen_fd_);
      if (io_monitor_)
      {
        io_monitor_->StopMonitor(listen_fd_);
      }
    }

    void Acceptor::AssignToReactor(int fd, const sockaddr_storage &peer)
    {
      NW_LOG_TRACE("[Acceptor::AssignToReactor] 开始分配 fd=" << fd);

      if (reactors_.empty())
      {
        // 如果没有配置 Reactor，无法处理此连接
        NW_LOG_ERROR("[Acceptor::AssignToReactor] reactors_.empty()，关闭 fd=" << fd);
        close(fd);
        return;
      }

      // 使用轮询策略选择 Reactor
      size_t index = next_reactor_index_.fetch_add(1) % reactors_.size();
      NW_LOG_DEBUG("[Acceptor] reactors_.size="
                   << reactors_.size() << ", 选择的 reactor_index=" << index);
      auto reactor_weak = reactors_[index];

      // 使用 lock() 获取 shared_ptr，必须检查是否有效
      auto reactor = reactor_weak.lock();
      if (!reactor)
      {
        // Reactor 已被销毁，无法处理此连接
        NW_LOG_ERROR("[Acceptor::AssignToReactor] Reactor 无效，关闭 fd=" << fd);
        close(fd);
        return;
      }

      NW_LOG_DEBUG("[Acceptor] 将 fd=" << fd << " 添加到 Reactor (reactor_id="
                                       << reactor->GetReactorId() << ")");

      // 将 fd 添加到 Reactor（Reactor 内部会提交 kConnected 事件）
      // 注意：AddConnection 现在是异步的，connection_id 将在 Reactor 线程中生成
      bool success = reactor->AddConnection(fd, peer);
      if (!success)
      {
        NW_LOG_ERROR(
            "[Acceptor::AssignToReactor] Reactor::AddConnection 失败，关闭 fd="
            << fd);
        close(fd);
      }
      else
      {
        NW_LOG_DEBUG(
            "[Acceptor] Reactor::AddConnection 成功，等待 kConnected 事件");
      }
    }
  } // namespace network
} // namespace darwincore