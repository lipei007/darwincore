//
// DarwinCore Network 模块
// 网络服务器 - 统一服务器接口
//
// 功能说明：
//   提供统一的网络服务器接口，支持 IPv4、IPv6、双栈监听和 Unix Domain Socket。
//   内部使用 Acceptor-Worker-Reactor 架构，自动管理线程池和事件分发。
//
// 设计规则：
//   - Server 是用户唯一需要关心的接口
//   - 内部实现（Acceptor、Reactor、Worker）对用户透明
//   - 使用回调函数处理网络事件
//
// 使用示例：
//   @code
//   darwincore::network::Server server;
//
//   // 设置事件回调
//   server.SetOnClientConnected([](const auto& info) {
//     std::cout << "客户端已连接: " << info.peer_address << std::endl;
//   });
//
//   server.SetOnMessage([](uint64_t conn_id, const auto& data) {
//     // 处理接收到的数据
//   });
//
//   // 启动服务器（IPv4）
//   server.StartIPv4("0.0.0.0", 8080);
//
//   // ... 服务器运行 ...
//
//   server.Stop();
//   @endcode
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#ifndef DARWINCORE_NETWORK_SERVER_H
#define DARWINCORE_NETWORK_SERVER_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <darwincore/network/event.h>

namespace darwincore {
namespace network {

/**
 * @brief 网络服务器类
 *
 * 提供统一的服务器接口，支持多种 Socket 类型。
 * 内部自动管理 Acceptor（监听）、Reactor 线程池（I/O）、Worker 线程池（业务）。
 *
 * 支持的启动方式：
 * - StartIPv4: 仅监听 IPv4
 * - StartIPv6: 仅监听 IPv6（不映射 IPv4）
 * - StartUniversalIP: 双栈监听（创建两个独立 socket）
 * - StartUnixDomain: Unix Domain Socket（本地 IPC）
 *
 * 线程模型：
 * - 1 个 Acceptor 线程：负责 accept 新连接
 * - N 个 Reactor 线程：负责 I/O（默认 N = CPU 核数）
 * - M 个 Worker 线程：负责业务逻辑（可配置）
 *
 * 资源管理：
 * - 所有 fd 由 Reactor 独占管理
 * - 回调函数在 Worker 线程中调用
 * - 业务层只能看到 connection_id，看不到 fd
 */
class Server {
public:
  struct Options {
    size_t reactor_count{0};     // 0 表示使用 CPU 核数
    size_t worker_count{4};
    size_t worker_queue_size{10000}; // 每个 worker 队列容量
  };

  struct ReactorStatistics {
    int reactor_id{0};
    uint64_t total_connections{0};
    uint64_t active_connections{0};
    uint64_t total_bytes_sent{0};
    uint64_t total_bytes_received{0};
    uint64_t total_ops_processed{0};
    uint64_t total_dispatch_failures{0};
    uint64_t total_add_requests{0};
    uint64_t total_add_failures{0};
    uint64_t total_remove_requests{0};
    uint64_t total_remove_failures{0};
    uint64_t total_send_requests{0};
    uint64_t total_send_failures{0};
    size_t pending_operation_queue_size{0};
  };

  struct Statistics {
    bool is_running{false};
    uint64_t total_connections{0};
    uint64_t active_connections{0};
    uint64_t total_messages_received{0};
    uint64_t total_messages_sent{0};
    uint64_t total_bytes_received{0};
    uint64_t total_bytes_sent{0};
    uint64_t total_errors{0};
    uint64_t worker_submit_failures{0};
    size_t worker_total_queue_size{0};
    std::vector<ReactorStatistics> reactors;
  };

  /// 客户端连接回调函数类型
  using OnClientConnectedCallback =
      std::function<void(const ConnectionInformation&)>;

  /// 消息接收回调函数类型
  /// @param connection_id 连接 ID
  /// @param data 接收到的数据
  using OnMessageCallback =
      std::function<void(uint64_t connection_id,
                         const std::vector<uint8_t>& data)>;

  /// 客户端断开回调函数类型
  /// @param connection_id 连接 ID
  using OnClientDisconnectedCallback = std::function<void(uint64_t connection_id)>;

  /// 连接错误回调函数类型
  /// @param connection_id 连接 ID
  /// @param error 错误类型
  /// @param message 错误消息（仅用于日志）
  using OnConnectionErrorCallback =
      std::function<void(uint64_t connection_id,
                         NetworkError error,
                         const std::string& message)>;

  /**
   * @brief 构造 Server 对象
   *
   * 默认创建 CPU 核数个 Reactor 线程和 4 个 Worker 线程。
   */
  Server();
  explicit Server(const Options& options);

  /**
   * @brief 析构函数
   *
   * 自动停止服务器并清理资源。
   */
  ~Server();

  // 禁止拷贝和移动
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // ==================== 启动服务器 ====================

  /**
   * @brief 启动 IPv4 服务器
   * @param host 监听地址（如 "0.0.0.0" 表示所有接口）
   * @param port 监听端口
   * @return 启动成功返回 true，失败返回 false
   *
   * 使用 IPv4 socket 监听指定地址和端口。
   * 使用系统默认的 backlog (SOMAXCONN)。
   * 自动设置 SO_REUSEADDR 和 SO_REUSEPORT。
   * 如果服务器已启动，此方法会失败。
   */
  bool StartIPv4(const std::string& host, uint16_t port);

  /**
   * @brief 启动 IPv6 服务器
   * @param host 监听地址（如 "::" 表示所有接口）
   * @param port 监听端口
   * @return 启动成功返回 true，失败返回 false
   *
   * 使用 IPv6 socket 监听指定地址和端口。
   * 使用系统默认的 backlog (SOMAXCONN)。
   * 设置 IPV6_V6ONLY=1，不接收 IPv4 映射连接。
   * 自动设置 SO_REUSEADDR 和 SO_REUSEPORT。
   */
  bool StartIPv6(const std::string& host, uint16_t port);

  /**
   * @brief 启动双栈服务器（IPv4 + IPv6）
   * @param host 监听地址
   * @param port 监听端口
   * @return 启动成功返回 true，失败返回 false
   *
   * 创建两个独立的 socket（一个 IPv4，一个 IPv6）同时监听。
   * 各自独立接受连接，由 Reactor 线程池管理。
   * 使用系统默认的 backlog (SOMAXCONN)。
   */
  bool StartUniversalIP(const std::string& host, uint16_t port);

  /**
   * @brief 启动 Unix Domain Socket 服务器
   * @param path Socket 文件路径（绝对路径推荐）
   * @return 启动成功返回 true，失败返回 false
   *
   * 创建 Unix Domain Socket 并绑定到指定路径。
   * 使用系统默认的 backlog (SOMAXCONN)。
   * 如果路径长度超过 sun_path 上限（104 bytes），
   * 会自动使用受控 chdir 策略处理。
   */
  bool StartUnixDomain(const std::string& path);

  /**
   * @brief 停止服务器
   *
   * 停止监听，关闭所有连接，停止所有工作线程。
   * 此方法会阻塞直到所有线程停止。
   */
  void Stop();

  // ==================== 发送数据 ====================

  /**
   * @brief 向指定连接发送数据
   * @param connection_id 连接 ID
   * @param data 数据指针
   * @param size 数据大小
   * @return 发送成功返回 true，失败返回 false
   *
   * 此方法可以从任何线程调用（包括回调函数）。
   * 数据会在对应的 Reactor 线程中实际发送。
   */
  bool SendData(uint64_t connection_id,
                const uint8_t* data,
                size_t size);

  // ==================== 设置回调 ====================

  /**
   * @brief 设置客户端连接回调
   * @param callback 连接建立时调用的函数
   *
   * 当新客户端连接成功后，此回调会在 Worker 线程中被调用。
   * 回调参数包含连接信息（不含 fd）。
   */
  void SetOnClientConnected(OnClientConnectedCallback callback);

  /**
   * @brief 设置消息接收回调
   * @param callback 接收到数据时调用的函数
   *
   * 当从客户端接收到数据后，此回调会在 Worker 线程中被调用。
   * 注意：此回调应该快速处理，避免阻塞 Worker 线程。
   */
  void SetOnMessage(OnMessageCallback callback);

  /**
   * @brief 设置客户端断开回调
   * @param callback 连接断开时调用的函数
   *
   * 当客户端断开连接后，此回调会在 Worker 线程中被调用。
   */
  void SetOnClientDisconnected(OnClientDisconnectedCallback callback);

  /**
   * @brief 设置连接错误回调
   * @param callback 连接发生错误时调用的函数
   *
   * 当连接发生错误后，此回调会在 Worker 线程中被调用。
   * 错误消息仅用于日志记录，业务决策应使用 NetworkError 枚举。
   */
  void SetOnConnectionError(OnConnectionErrorCallback callback);

  /**
   * @brief 获取运行时统计信息
   *
   * 可用于观测负载、吞吐和排队压力。
   */
  Statistics GetStatistics() const;

private:
  /// Pimpl 实现（隐藏内部细节）
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace network
}  // namespace darwincore

#endif  // DARWINCORE_NETWORK_SERVER_H
