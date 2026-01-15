//
// DarwinCore Network 模块
// Acceptor - 接收器（仅 Server 使用）
//
// 功能说明：
//   Acceptor 负责监听 Socket 并接受新的客户端连接。
//   接受新连接后，将 fd 分配给 Reactor 线程池。
//
// 设计规则：
//   - Acceptor 只负责 listen/accept
//   - 不进行任何 I/O 操作（read/write）
//   - accept 后立即将 fd 交给 Reactor
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#ifndef DARWINCORE_NETWORK_ACCEPTOR_H
#define DARWINCORE_NETWORK_ACCEPTOR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <sys/socket.h>
#include <thread>

#include <darwincore/network/configuration.h> // 对外头文件

namespace darwincore {
namespace network {

// 前向声明
class Reactor;
class IOMonitor;

/**
 * @brief Acceptor - 服务器监听器
 *
 * Acceptor 负责监听 Socket 并接受新的客户端连接。
 * 接收到新连接后，会使用轮询策略将 fd 分配给 Reactor 线程。
 *
 * 线程模型：
 *   - Acceptor 在独立线程中运行
 *   - 使用阻塞式 accept
 *   - 接受连接后立即分配给 Reactor
 *
 * 支持的 Socket 类型：
 *   - IPv4: socket(AF_INET, SOCK_STREAM, 0)
 *   - IPv6: socket(AF_INET6, SOCK_STREAM, 0)
 *   - UnixDomain: socket(AF_UNIX, SOCK_STREAM, 0)
 *
 * 注意事项：
 *   - Acceptor 不进行任何 I/O 操作
 *   - 接收到的 fd 立即交由 Reactor 管理
 *   - Client 不使用 Acceptor
 */
class Acceptor {
public:
  /**
   * @brief 构造 Acceptor 对象
   */
  Acceptor();

  /**
   * @brief 析构函数
   *
   * 自动停止监听并关闭监听 Socket。
   */
  ~Acceptor();

  // 禁止拷贝和移动
  Acceptor(const Acceptor &) = delete;
  Acceptor &operator=(const Acceptor &) = delete;

  // ==================== 启动监听 ====================

  /**
   * @brief 启动 IPv4 监听
   * @param host 监听地址（如 "0.0.0.0"）
   * @param port 监听端口
   * @return 启动成功返回 true，失败返回 false
   *
   * 创建 IPv4 socket 并开始监听。
   * 使用系统默认的 backlog (SOMAXCONN)。
   * 自动设置 SO_REUSEADDR 和 SO_REUSEPORT。
   * 新连接会自动分配给 Reactor，通过 kConnected 事件通知。
   */
  bool ListenIPv4(const std::string &host, uint16_t port);

  /**
   * @brief 启动 IPv6 监听
   * @param host 监听地址（如 "::"）
   * @param port 监听端口
   * @return 启动成功返回 true，失败返回 false
   *
   * 创建 IPv6 socket 并开始监听。
   * 使用系统默认的 backlog (SOMAXCONN)。
   * 设置 IPV6_V6ONLY=1，不接收 IPv4 映射连接。
   * 自动设置 SO_REUSEADDR 和 SO_REUSEPORT。
   */
  bool ListenIPv6(const std::string &host, uint16_t port);

  /**
   * @brief 启动 Unix Domain Socket 监听
   * @param path Socket 文件路径（绝对路径推荐）
   * @return 启动成功返回 true，失败返回 false
   *
   * 创建 Unix Domain Socket 并开始监听。
   * 使用系统默认的 backlog (SOMAXCONN)。
   * 如果路径长度超过 sun_path 上限，会使用受控 chdir 策略。
   */
  bool ListenUnixDomain(const std::string &path);

  // ==================== 设置 Reactor 线程池 ====================

  /**
   * @brief 设置 Reactor 线程池
   * @param reactors Reactor 线程列表（weak_ptr，非 owning）
   *
   * Acceptor 使用轮询策略将新连接分配给不同的 Reactor。
   * 使用 weak_ptr 避免所有权循环，确保：
   *   - Acceptor 不会延长 Reactor 生命周期
   *   - Server 作为 Owner 掌控 Reactor 销毁时机
   *   - 使用前必须通过 lock() 检查 Reactor 是否仍然有效
   */
  void SetReactors(const std::vector<std::weak_ptr<Reactor>> &reactors);

  // ==================== 停止监听 ====================

  /**
   * @brief 停止监听
   *
   * 关闭监听 Socket，停止接受新连接。
   * 此方法会阻塞直到 accept 线程停止。
   */
  void Stop();

  /**
   * @brief 检查是否正在监听
   * @return 正在监听返回 true，否则返回 false
   */
  bool IsRunning() const;

private:
  /**
   * @brief 监听 Socket 的通用实现
   * @param protocol Socket 协议类型
   * @param host 监听地址
   * @param port 监听端口（Unix Domain 时为 0）
   * @return 启动成功返回 true，失败返回 false
   *
   * 使用系统默认的 backlog (SOMAXCONN)。
   * 自动设置 SO_REUSEADDR 和 SO_REUSEPORT。
   */
  bool ListenGeneric(SocketProtocol protocol, const std::string &host,
                     uint16_t port);

  /**
   * @brief Accept 线程的主循环
   *
   * 在独立线程中持续运行，接受新连接并分配给 Reactor。
   */
  void AcceptLoop();

  /**
   * @brief 将新连接分配给下一个 Reactor
   * @param fd 新连接的文件描述符
   * @param peer 客户端地址
   */
  void AssignToReactor(int fd, const sockaddr_storage &peer);

private:
  int listen_fd_;                         ///< 监听 Socket 文件描述符
  std::unique_ptr<IOMonitor> io_monitor_; ///< IO 监控器（智能指针管理）
  std::thread accept_thread_;             ///< Accept 线程
  std::atomic<bool> is_running_{false}; ///< 运行状态

  std::vector<std::weak_ptr<Reactor>> reactors_; ///< Reactor 线程池（weak_ptr，非 owning）
  std::atomic<size_t> next_reactor_index_; ///< 下一个分配的 Reactor 索引（轮询）
};

} // namespace network
} // namespace darwincore

#endif // DARWINCORE_NETWORK_ACCEPTOR_H
