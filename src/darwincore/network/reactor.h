//
// DarwinCore Network 模块
// Reactor - IO 事件循环
//
// 作者: DarwinCore Network 团队
// 日期: 2026
//

#ifndef DARWINCORE_NETWORK_REACTOR_H
#define DARWINCORE_NETWORK_REACTOR_H

#include <atomic>
#include <cstdint>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include "concurrent_queue.h"
#include "send_buffer.h"
#include "connection_id_generator.h"
#include <darwincore/network/event.h>

namespace darwincore
{
  namespace network
  {

    // 前向声明
    class WorkerPool;
    class IOMonitor;

    /**
     * @brief Reactor - IO 事件循环
     *
     * Reactor 在专用线程中运行，负责：
     *  - socket 读写事件监听
     *  - 连接生命周期管理
     *  - 网络事件分发
     *  - 超时检测与统计
     *
     * 线程模型：
     *  - Reactor 内部状态仅由 Reactor 线程访问
     *  - 公共接口通过线程安全操作队列与 Reactor 线程交互
     */
    class Reactor
    {
    public:
      using EventCallback = std::function<void(const NetworkEvent &)>;

      struct Statistics
      {
        int reactor_id{0};
        uint64_t total_connections{0};
        uint64_t active_connections{0};
        uint64_t total_bytes_sent{0};
        uint64_t total_bytes_received{0};
        uint64_t total_ops_processed{0};
      };

      Reactor(int id, const std::shared_ptr<WorkerPool> &worker_pool);
      ~Reactor();

      Reactor(const Reactor &) = delete;
      Reactor &operator=(const Reactor &) = delete;

      bool Start();
      void Stop();

      /**
       * @brief 添加连接（线程安全，同步等待结果）
       *
       * @return 成功返回 true，失败返回 false
       *
       * 注意：
       *   - connection_id 在 Reactor 线程中生成
       *   - 调用线程会等待 Reactor 处理完成
       */
      bool AddConnection(int fd, const sockaddr_storage &peer);

      bool RemoveConnection(uint64_t connection_id);

      bool SendData(uint64_t connection_id,
                    const uint8_t *data,
                    size_t size);

      /**
       * @brief 获取连接的发送缓冲区大小
       * @param connection_id 连接ID
       * @return 发送缓冲区大小（字节），连接不存在返回0
       */
      size_t GetSendBufferSize(uint64_t connection_id) const;

      void SetEventCallback(EventCallback callback);

      void SetConnectionTimeout(std::chrono::seconds timeout);

      Statistics GetStatistics() const;

      int GetReactorId() const { return reactor_id_; }

    private:
      // ============ 内部结构 ============

      struct ReactorConnection
      {
        int file_descriptor{-1};
        sockaddr_storage peer_address{};
        uint64_t connection_id{0};

        SendBuffer send_buffer;
        bool read_paused{false};
        bool write_pending{false};

        std::chrono::steady_clock::time_point last_active;

        ReactorConnection(int fd,
                          const sockaddr_storage &peer,
                          uint64_t id);

        void UpdateActivity();
        bool IsTimeout(std::chrono::seconds timeout) const;

        ReactorConnection(const ReactorConnection &) = delete;
        ReactorConnection &operator=(const ReactorConnection &) = delete;
      };

      struct Operation
      {
        enum Type
        {
          kAdd,
          kRemove,
          kSend
        };

        Type type{kAdd};
        int fd{-1};
        uint64_t connection_id{0};
        sockaddr_storage peer{};
        std::vector<uint8_t> data;

        std::shared_ptr<std::promise<uint64_t>> promise;
      };

      // ============ Reactor 线程私有方法 ============

      void RunEventLoop();
      void ProcessPendingOperations();

      uint64_t DoAddConnection(int fd, const sockaddr_storage &peer);
      bool DoRemoveConnection(uint64_t connection_id);
      bool DoSendData(uint64_t connection_id,
                      const std::vector<uint8_t> &data);

      bool TrySendDirect(int fd,
                         const uint8_t *data,
                         size_t size,
                         size_t &sent,
                         bool &error);

      bool BufferAndMonitorWrite(ReactorConnection &conn,
                                 const uint8_t *data,
                                 size_t size);

      void ProcessKqueueEvent(const struct kevent &event);

      void HandleReadEvent(int fd);
      void HandleWriteEvent(int fd);

      void CheckTimeouts();

      void HandleConnectionClose(const ReactorConnection &conn);
      void HandleConnectionError(const ReactorConnection &conn,
                                 int error_code);

      void CleanupAllConnections();

      // ============ 事件分发 ============

      void DispatchEvent(const NetworkEvent &event);

      void DispatchConnectionEvent(uint64_t connection_id,
                                   const sockaddr_storage &peer);

      void DispatchDataEvent(uint64_t connection_id,
                             const uint8_t *data,
                             size_t size);

      void DispatchDisconnectEvent(uint64_t connection_id);

      void DispatchErrorEvent(uint64_t connection_id,
                              int error_code);

      NetworkError MapErrnoToNetworkError(int errno_val);

      // ============ 成员变量 ============

      int reactor_id_;
      std::shared_ptr<WorkerPool> worker_pool_;
      EventCallback event_callback_;

      std::unique_ptr<IOMonitor> io_monitor_;
      std::thread event_loop_thread_;
      std::atomic<bool> is_running_{false};

      std::atomic<uint16_t> connection_seq_{0};

      std::unordered_map<uint64_t, ReactorConnection> connections_;
      std::unordered_map<int, uint64_t> fd_to_connection_id_;

      ConcurrentQueue<Operation> pending_operations_;

      std::chrono::seconds connection_timeout_;

      // 统计
      std::atomic<uint64_t> total_connections_{0};
      std::atomic<uint64_t> active_connections_{0};
      std::atomic<uint64_t> total_bytes_sent_{0};
      std::atomic<uint64_t> total_bytes_received_{0};
      std::atomic<uint64_t> total_ops_processed_{0};
    };

  } // namespace network
} // namespace darwincore

#endif // DARWINCORE_NETWORK_REACTOR_H
