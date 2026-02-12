//
// DarwinCore Network 模块
// ClientReactor - 客户端专用 I/O 事件循环
//
// 功能说明：
//   ClientReactor 是专门为客户端优化的单连接 Reactor。
//   与通用 Reactor 不同，ClientReactor 专注于单个连接的 I/O 管理。
//
// 核心特性：
//   - 单连接管理（只需维护一个 fd）
//   - 同步发送支持（SendSync - 等待发送完成）
//   - 异步发送支持（SendAsync - 放入缓冲区后立即返回）
//   - 连接状态管理
//   - 简化的背压控制
//
// 设计原则：
//   - 专用于 Client，不与 Server 共享
//   - 简化的事件循环（单连接）
//   - 支持同步/异步双模式发送
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#ifndef DARWINCORE_NETWORK_CLIENT_REACTOR_H
#define DARWINCORE_NETWORK_CLIENT_REACTOR_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/socket.h>

#include "send_buffer.h"
#include "concurrent_queue.h"
#include <darwincore/network/event.h>

namespace darwincore
{
  namespace network
  {

    // 前向声明
    class WorkerPool;
    class IOMonitor;

    /**
     * @brief ClientReactor - 客户端专用 Reactor
     *
     * 专门为客户端优化的单连接 Reactor，支持：
     *   - 同步发送（SendSync）：等待数据实际发送完成
     *   - 异步发送（SendAsync）：放入缓冲区后立即返回
     *   - 单连接管理：简化的事件循环
     *
     * 线程模型：
     *   - ClientReactor 运行在独立线程中
     *   - 公共接口通过操作队列与 Reactor 线程交互
     *   - 事件回调在 Worker 线程中执行
     */
    class ClientReactor
    {
    public:
      /// 发送完成回调函数类型
      using SendCompleteCallback = std::function<void(bool success, size_t sent)>;

      /// 统计信息
      struct Statistics
      {
        uint64_t total_bytes_sent{0};
        uint64_t total_bytes_received{0};
        uint64_t pending_send_operations{0};
        size_t send_buffer_size{0};
      };

      /**
       * @brief 构造 ClientReactor
       * @param worker_pool Worker 线程池（用于事件回调）
       */
      explicit ClientReactor(const std::shared_ptr<WorkerPool> &worker_pool);

      /**
       * @brief 析构函数
       *
       * 自动停止事件循环并清理资源。
       */
      ~ClientReactor();

      // 禁止拷贝和移动
      ClientReactor(const ClientReactor &) = delete;
      ClientReactor &operator=(const ClientReactor &) = delete;

      // ==================== 生命周期管理 ====================

      /**
       * @brief 启动 ClientReactor
       * @return 成功返回 true，失败返回 false
       */
      bool Start();

      /**
       * @brief 停止 ClientReactor
       *
       * 停止事件循环，关闭连接。
       */
      void Stop();

      // ==================== 连接管理 ====================

      /**
       * @brief 设置连接（ClientReactor 只管理一个连接）
       * @param fd 文件描述符
       * @param peer 对端地址
       * @return 成功返回 true，失败返回 false
       *
       * 注意：此方法需要在 Start() 之后调用。
       */
      bool SetConnection(int fd, const sockaddr_storage &peer);

      /**
       * @brief 移除连接
       */
      void RemoveConnection();

      // ==================== 发送数据 ====================

      /**
       * @brief 异步发送数据（默认模式）
       * @param data 数据指针
       * @param size 数据大小
       * @return 成功返回 true，失败返回 false
       *
       * 数据放入发送缓冲区后立即返回，不等待实际发送完成。
       * 返回 true 只表示数据已放入缓冲区，不表示已发送到对端。
       */
      bool SendAsync(const uint8_t *data, size_t size);

      /**
       * @brief 同步发送数据（等待发送完成）
       * @param data 数据指针
       * @param size 数据大小
       * @param timeout_ms 超时时间（毫秒），0 表示无限等待
       * @return 成功返回 true，失败/超时返回 false
       *
       * 此方法会等待数据实际发送到对端后返回。
       * 支持超时控制。
       */
      bool SendSync(const uint8_t *data, size_t size, int timeout_ms = 0);

      /**
       * @brief 异步发送（带完成回调）
       * @param data 数据指针
       * @param size 数据大小
       * @param callback 发送完成回调（在 Reactor 线程中调用）
       * @return 成功返回 true，失败返回 false
       */
      bool SendAsyncWithCallback(const uint8_t *data, size_t size,
                                 SendCompleteCallback callback);

      // ==================== 状态查询 ====================

      /**
       * @brief 获取发送缓冲区大小
       * @return 发送缓冲区中的字节数
       */
      size_t GetSendBufferSize() const;

      /**
       * @brief 检查是否已连接
       * @return 已连接返回 true，否则返回 false
       */
      bool IsConnected() const;

      /**
       * @brief 获取统计信息
       * @return 统计信息结构
       */
      Statistics GetStatistics() const;

      // ==================== 事件回调 ====================

      /**
       * @brief 设置事件回调
       * @param callback 事件回调函数
       *
       * 当有网络事件发生时，此回调会在 Worker 线程中被调用。
       */
      void SetEventCallback(std::function<void(const NetworkEvent &)> callback);

    private:
      // ============ 内部结构 ============

      /**
       * @brief 连接信息（ClientReactor 只管理一个连接）
       */
      struct ClientConnection
      {
        int file_descriptor{-1};
        sockaddr_storage peer_address{};
        uint64_t connection_id{0};
        SendBuffer send_buffer;
        bool write_pending{false};

        std::chrono::steady_clock::time_point last_active;

        ClientConnection() = default;

        void UpdateActivity()
        {
          last_active = std::chrono::steady_clock::now();
        }
      };

      /**
       * @brief 发送操作
       */
      struct SendOperation
      {
        enum Type
        {
          kAsync,      // 异步发送
          kSync,       // 同步发送
          kAsyncCallback // 异步发送（带回调）
        };

        Type type{kAsync};
        std::vector<uint8_t> data;
        SendCompleteCallback callback;
        std::shared_ptr<std::promise<bool>> promise;
        std::shared_ptr<std::promise<size_t>> sent_promise;
      };

      // ============ Reactor 线程私有方法 ============

      void RunEventLoop();
      void ProcessPendingOperations();

      bool DoSend(const uint8_t *data, size_t size);
      void TrySendDirect();
      void HandleWriteEvent();

      void ProcessKqueueEvent(const struct kevent &event);
      void HandleReadEvent();

      void DispatchEvent(const NetworkEvent &event);
      void DispatchDataEvent(const uint8_t *data, size_t size);
      void DispatchConnectedEvent();
      void DispatchDisconnectedEvent();

      // ============ 成员变量 ============

      std::shared_ptr<WorkerPool> worker_pool_;
      std::function<void(const NetworkEvent &)> event_callback_;

      std::unique_ptr<IOMonitor> io_monitor_;
      std::thread event_loop_thread_;
      std::atomic<bool> is_running_{false};

      // 单连接管理
      ClientConnection connection_;
      std::atomic<bool> has_connection_{false};

      // 发送操作队列
      ConcurrentQueue<SendOperation> send_operations_;
      static constexpr uintptr_t kWakeupIdent = 2;

      // 统计
      std::atomic<uint64_t> total_bytes_sent_{0};
      std::atomic<uint64_t> total_bytes_received_{0};
    };

  } // namespace network
} // namespace darwincore

#endif // DARWINCORE_NETWORK_CLIENT_REACTOR_H
