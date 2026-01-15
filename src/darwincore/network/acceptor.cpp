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

// SOMAXCONN 可能在某些系统上未定义
#ifndef SOMAXCONN
#define SOMAXCONN 128
#endif

namespace darwincore {
    namespace network {
        Acceptor::Acceptor()
            : listen_fd_(-1), io_monitor_(std::make_unique<IOMonitor>()),
              next_reactor_index_(0) {
            io_monitor_->Initialize();
        }

        Acceptor::~Acceptor() {
            Stop();
            // io_monitor_ 由 unique_ptr 自动释放
        }

        bool Acceptor::ListenIPv4(const std::string &host, uint16_t port) {
            return ListenGeneric(SocketProtocol::kIPv4, host, port);
        }

        bool Acceptor::ListenIPv6(const std::string &host, uint16_t port) {
            return ListenGeneric(SocketProtocol::kIPv6, host, port);
        }

        bool Acceptor::ListenUnixDomain(const std::string &path) {
            return ListenGeneric(SocketProtocol::kUnixDomain, path, 0);
        }

        void Acceptor::SetReactors(
            const std::vector<std::weak_ptr<Reactor> > &reactors) {
            // 直接存储 weak_ptr，不延长生命周期
            reactors_ = reactors;
        }

        void Acceptor::Stop() {
            is_running_ = false;

            // 唤醒 accept 线程
            if (io_monitor_) {
                // 通过停止监控器来唤醒 kevent/epoll_wait
                // io_monitor_ 会在析构时自动清理资源
            }

            if (accept_thread_.joinable()) {
                accept_thread_.join();
            }

            if (listen_fd_ >= 0) {
                close(listen_fd_);
                listen_fd_ = -1;
            }
        }

        bool Acceptor::IsRunning() const { return is_running_; }

        bool Acceptor::ListenGeneric(SocketProtocol protocol, const std::string &host,
                                     uint16_t port) {
            // 创建 Socket
            int fd = SocketHelper::CreateSocket(protocol);
            if (fd < 0) {
                NW_LOG_ERROR("[Acceptor::ListenGeneric] CreateSocket 失败");
                return false;
            }

            // 设置非阻塞
            if (!SocketHelper::SetNonBlocking(fd)) {
                NW_LOG_ERROR("[Acceptor::ListenGeneric] SetNonBlocking 失败");
                close(fd);
                return false;
            }

            // 设置 SO_REUSEADDR (允许端口快速重用)
            int opt = 1;
            if (!SocketHelper::SetSocketOption(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
                NW_LOG_WARNING("[Acceptor::ListenGeneric] SO_REUSEADDR 设置失败");
                close(fd);
                return false;
            }

            // 设置 SO_REUSEPORT (允许多个 socket 监听同一端口)
            // 这对于负载均衡和多进程很有用
            if (!SocketHelper::SetSocketOption(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
                // 忽略失败，SO_REUSEPORT 不是所有平台都支持
                NW_LOG_DEBUG("[Acceptor::ListenGeneric] SO_REUSEPORT 设置失败（可能不支持）");
            }

            // 解析并绑定地址
            sockaddr_storage addr = {};
            if (!SocketHelper::ResolveAddress(host, port, protocol, &addr)) {
                NW_LOG_ERROR("[Acceptor::ListenGeneric] ResolveAddress 失败: " << host);
                close(fd);
                return false;
            }

            // 对于 Unix Domain Socket，在 bind 之前清理旧文件
            if (protocol == SocketProtocol::kUnixDomain) {
                SocketHelper::UnlinkUnixDomainSocket(host);
            }

            // 计算正确的地址长度
            socklen_t addr_len = 0;
            switch (addr.ss_family) {
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

            if (bind(fd, reinterpret_cast<const sockaddr *>(&addr), addr_len) < 0) {
                NW_LOG_ERROR("[Acceptor::ListenGeneric] bind 失败: " << strerror(errno)
                    << ", fd=" << fd);
                close(fd);
                return false;
            }

            // 开始监听
            // 使用系统默认的 backlog (SOMAXCONN)
            // 这已经是平台优化的值，通常在 128-4096 之间
            if (listen(fd, SOMAXCONN) < 0) {
                NW_LOG_ERROR("[Acceptor::ListenGeneric] listen 失败: " << strerror(errno)
                    << ", fd=" << fd);
                close(fd);
                return false;
            }

            listen_fd_ = fd;
            NW_LOG_INFO("[Acceptor::ListenGeneric] 监听成功: fd="
                << fd << ", backlog=" << SOMAXCONN << ", SO_REUSEADDR=1"
                << ", SO_REUSEPORT=1"
                << ", protocol=" << static_cast<int>(protocol));

            is_running_ = true;
            accept_thread_ = std::thread(&Acceptor::AcceptLoop, this);

            return true;
        }

        void Acceptor::AcceptLoop() {
            NW_LOG_DEBUG("[Acceptor::AcceptLoop] AcceptLoop 启动，listen_fd=" << listen_fd_);

            if (!io_monitor_) {
                NW_LOG_ERROR("[Acceptor::AcceptLoop] io_monitor_ 为空");
                return;
            }

            // 监听 listen_fd 的可读事件
            if (!io_monitor_->StartReadMonitor(listen_fd_)) {
                NW_LOG_ERROR("[Acceptor::AcceptLoop] 启动监控 listen_fd 失败");
                return;
            }

            NW_LOG_INFO("[Acceptor::AcceptLoop] 开始监听连接...");

#if USE_KQUEUE
            const int kMaxEvents = 32;
            struct kevent events[kMaxEvents];
#else
  const int kMaxEvents = 32;
  struct epoll_event events[kMaxEvents];
#endif

            while (is_running_) {
                // 等待事件，设置 100ms 超时以便检查 is_running_
                int timeout_ms = 100;
                int nev = io_monitor_->WaitEvents(events, kMaxEvents, &timeout_ms);

                if (nev < 0) {
                    if (errno == EINTR) {
                        NW_LOG_TRACE("[Acceptor::AcceptLoop] 被信号中断，继续等待");
                        continue; // 被信号中断
                    }
                    NW_LOG_ERROR(
                        "[Acceptor::AcceptLoop] WaitEvents 出错: " << strerror(errno));
                    break; // 其他错误，退出循环
                }

                for (int i = 0; i < nev; ++i) {
#if USE_KQUEUE
                    int fd = events[i].ident;
#else
      int fd = events[i].data.fd;
#endif

                    if (fd == listen_fd_) {
                        // 有新连接到达
                        sockaddr_storage peer_addr = {};
                        socklen_t peer_len = sizeof(peer_addr);

                        int client_fd = accept(
                            listen_fd_, reinterpret_cast<sockaddr *>(&peer_addr), &peer_len);

                        if (client_fd < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                continue; // 没有连接了，继续等待
                            }
                            NW_LOG_ERROR(
                                "[Acceptor::AcceptLoop] accept 失败: " << strerror(errno));
                            // 其他错误，退出循环
                            break;
                        }

                        NW_LOG_TRACE(
                            "[Acceptor::AcceptLoop] 接受新连接，client_fd=" << client_fd);

                        // 设置客户端 Socket 为非阻塞
                        if (!SocketHelper::SetNonBlocking(client_fd)) {
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
                    }
                }
            }

            // 停止监听 listen_fd
            NW_LOG_DEBUG("[Acceptor::AcceptLoop] AcceptLoop 结束，停止监听 listen_fd="
                << listen_fd_);
            io_monitor_->StopMonitor(listen_fd_);
        }

        void Acceptor::AssignToReactor(int fd, const sockaddr_storage &peer) {
            NW_LOG_TRACE("[Acceptor::AssignToReactor] 开始分配 fd=" << fd);

            if (reactors_.empty()) {
                // 如果没有配置 Reactor，无法处理此连接
                NW_LOG_ERROR(
                    "[Acceptor::AssignToReactor] reactors_.empty()，关闭 fd=" << fd);
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
            if (!reactor) {
                // Reactor 已被销毁，无法处理此连接
                NW_LOG_ERROR("[Acceptor::AssignToReactor] Reactor 无效，关闭 fd=" << fd);
                close(fd);
                return;
            }

            NW_LOG_DEBUG("[Acceptor] 将 fd=" << fd << " 添加到 Reactor (reactor_id="
                << reactor->GetReactorId() << ")");
            // 将 fd 添加到 Reactor（Reactor 内部会提交 kConnected 事件）
            uint64_t connection_id = reactor->AddConnection(fd, peer);
            if (connection_id == 0) {
                NW_LOG_ERROR(
                    "[Acceptor::AssignToReactor] Reactor::AddConnection 失败，关闭 fd="
                    << fd);
                close(fd);
            } else {
                NW_LOG_DEBUG(
                    "[Acceptor] Reactor::AddConnection 成功，conn_id=" << connection_id);
            }
        }
    } // namespace network
} // namespace darwincore
