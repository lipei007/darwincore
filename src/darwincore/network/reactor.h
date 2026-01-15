//
// DarwinCore Network 模块
// Reactor - IO 事件循环
//
// 功能说明：
//   Reactor 负责管理文件描述符和 I/O 事件。
//   在自己的线程中运行，使用 kqueue (macOS/FreeBSD) 或 epoll (Linux)。
//
// 设计规则：
//   - 每个 fd 仅属于一个 Reactor 线程
//   - Reactor 执行所有 socket 操作（读、写、关闭）
//   - 将 I/O 结果转换为 NetworkEvents 并转发给 WorkerPool
//   - 绝不与 Worker 线程或业务逻辑共享 fd
//   - 外部线程通过操作队列安全地与 Reactor 交互
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#ifndef DARWINCORE_NETWORK_REACTOR_H
#define DARWINCORE_NETWORK_REACTOR_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include "concurrent_queue.h"         // 线程安全队列
#include <darwincore/network/event.h> // 对外暴露的头文件

namespace darwincore {
namespace network {

// 前向声明
class WorkerPool;
class IOMonitor;

/**
 * @brief Reactor - IO 事件循环
 *
 * Reactor 在专用线程中管理文件描述符和 I/O 事件。
 * 职责包括：
 *   - 添加/移除连接
 *   - 从 sockets 读/写数据
 *   - 将 I/O 事件转换为 NetworkEvents
 *   - 将事件转发给 WorkerPool 进行业务逻辑处理
 *
 * 线程安全：
 *   - Reactor 在自己的线程中运行
 *   - 公共方法是线程安全的（通过操作队列实现）
 *   - 私有方法仅从 Reactor 线程调用
 */
class Reactor {
public:
  /// 事件回调类型别名
  using EventCallback = std::function<void(const NetworkEvent &)>;

  /**
   * @brief 构造新的 Reactor 对象
   * @param id Reactor ID（用于日志/调试）
   * @param worker_pool WorkerPool 的共享指针，用于事件转发
   */
  Reactor(int id, const std::shared_ptr<WorkerPool> &worker_pool);

  /// 析构函数 - 停止事件循环
  ~Reactor();

  // 不可拷贝和不可移动
  Reactor(const Reactor &) = delete;
  Reactor &operator=(const Reactor &) = delete;

  /**
   * @brief 启动 reactor 事件循环
   * @return 启动成功返回 true，否则返回 false
   */
  bool Start();

  /**
   * @brief 停止 reactor 事件循环
   */
  void Stop();

  /**
   * @brief 向 reactor 添加新连接（线程安全）
   * @param fd 连接的文件描述符
   * @param peer 对端地址信息
   * @return connection_id（如果添加成功），0（如果失败）
   *
   * 连接ID格式：| 32位: YYYYMMDD + reactor_id | 32位: 每日递增序号 |
   */
  uint64_t AddConnection(int fd, const sockaddr_storage &peer);

  /**
   * @brief 从 reactor 中移除连接（线程安全）
   * @param connection_id 要移除的连接 ID
   * @return 移除成功返回 true，否则返回 false
   */
  bool RemoveConnection(uint64_t connection_id);

  /**
   * @brief 向连接发送数据（线程安全，异步非阻塞）
   * @param connection_id 发送数据的连接 ID
   * @param data 数据缓冲区指针
   * @param size 要发送的数据大小
   * @return 发送成功返回 true，否则返回 false
   */
  bool SendData(uint64_t connection_id, const uint8_t *data, size_t size);

  /**
   * @brief 设置接收网络事件的事件回调
   * @param callback 当网络事件发生时调用的函数
   */
  void SetEventCallback(EventCallback callback);

  /**
   * @brief 获取 reactor ID
   * @return Reactor ID
   */
  int GetReactorId() const { return reactor_id_; }

private:
  // ============ 内部结构 ============

  /// 连接结构（包含发送缓冲区）
  struct ReactorConnection {
    int file_descriptor{-1};
    sockaddr_storage peer{};
    uint64_t connection_id{0};
    std::vector<uint8_t> send_buffer; // 发送缓冲区
    bool write_pending{false};        // 是否正在等待可写事件

    ReactorConnection() { memset(&peer, 0, sizeof(peer)); }

    ReactorConnection(int fd, const sockaddr_storage &p, uint64_t id)
        : file_descriptor(fd), peer(p), connection_id(id) {}
  };

  /// 操作类型（用于线程安全的操作队列）
  struct Operation {
    enum Type { kAdd, kRemove, kSend };
    Type type{kAdd};
    int fd{-1};
    uint64_t connection_id{0};
    std::vector<uint8_t> data;
    sockaddr_storage peer{};
  };

  // ============ 私有方法（仅在 Reactor 线程执行）============

  /// 主事件循环
  void RunEventLoop();

  /// 处理待执行的操作队列
  void ProcessPendingOperations();

  /// 实际执行添加连接
  uint64_t DoAddConnection(int fd, const sockaddr_storage &peer);

  /// 实际执行移除连接
  bool DoRemoveConnection(uint64_t connection_id);

  /// 实际执行发送数据
  bool DoSendData(uint64_t connection_id, const std::vector<uint8_t> &data);

  /// 处理读事件
  void HandleReadEvent(int file_descriptor);

  /// 处理写事件（发送缓冲区数据）
  void HandleWriteEvent(int file_descriptor);

  /// 处理连接关闭事件
  void HandleConnectionClose(const ReactorConnection &connection);

  /// 处理连接错误事件
  void HandleConnectionError(const ReactorConnection &connection,
                             int error_code);

  /// 将系统 errno 映射到 NetworkError 枚举
  NetworkError MapErrnoToNetworkError(int errno_val);

  /// 生成连接ID（格式：YYYYMMDD + reactor_id + 序号）
  uint64_t GenerateConnectionId();

  // ============ 成员变量 ============

  int reactor_id_;                          ///< Reactor ID 用于日志
  std::shared_ptr<WorkerPool> worker_pool_; ///< WorkerPool 用于事件转发
  EventCallback event_callback_;            ///< 事件回调函数

  std::unique_ptr<IOMonitor> io_monitor_; ///< IO 监控器（智能指针管理）
  std::thread event_loop_thread_;         ///< 事件循环线程
  std::atomic<bool> is_running_{false}; ///< Reactor 运行状态

  // 连接ID生成
  std::atomic<uint32_t> daily_sequence_{0}; ///< 每日递增序号
  int cached_date_{0};                      ///< 缓存的日期 (YYYYMMDD)

  // 连接管理（仅在 Reactor 线程访问，无需加锁）
  std::unordered_map<uint64_t, ReactorConnection> connections_;
  std::unordered_map<int, uint64_t> fd_to_connection_id_;

  // 线程安全操作队列
  ConcurrentQueue<Operation> pending_operations_;
};

} // namespace network
} // namespace darwincore

#endif // DARWINCORE_NETWORK_REACTOR_H
