//
// DarwinCore Network 模块
// 工作线程池 - 业务逻辑处理
//
// 功能说明：
//   WorkerPool 管理多个工作线程池用于业务逻辑处理。
//   每个工作线程有独立的事件队列，处理来自 Reactor 的事件。
//
// 设计规则：
//   - Worker 永远不访问文件描述符（fd）
//   - Worker 只能看到 NetworkEvent 和 ConnectionInformation
//   - Worker 负责协议解析和业务逻辑处理
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#ifndef DARWINCORE_NETWORK_WORKER_POOL_H
#define DARWINCORE_NETWORK_WORKER_POOL_H

#include <cstdint>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <darwincore/network/event.h> // 对外暴露的头文件
#include "concurrent_queue.h"         // 内部头文件

namespace darwincore
{
  namespace network
  {

    /**
     * @brief 工作线程池 - 用于业务逻辑处理
     *
     * WorkerPool 管理多个工作线程，每个线程有自己独立的事件队列。
     * Reactor 使用轮询策略将 NetworkEvent 提交给池子。
     * Worker 处理事件并调用用户定义的回调函数。
     *
     * 线程安全：
     *   - SubmitEvent 可以从任何线程调用（包括 Reactor）
     *   - 每个 Worker 在自己的线程中运行
     *   - 事件队列是线程安全的
     *
     * 设计原则：
     *   - Worker 永远不访问文件描述符
     *   - Worker 只能看到 NetworkEvent（无 fd）
     *   - Worker 执行协议解析和业务逻辑
     */
    class WorkerPool
    {
    public:
      /// 事件回调函数类型别名
      using EventCallback = std::function<void(const NetworkEvent &)>;

      /**
       * @brief 构造一个新的 WorkerPool 对象
       * @param worker_count 要创建的工作线程数量
       * @param max_queue_size 每个队列的最大容量（0 = 无限制）
       */
      explicit WorkerPool(size_t worker_count, size_t max_queue_size = 0);

      /// 析构函数 - 停止所有工作线程
      ~WorkerPool();

      // 禁止拷贝和移动
      WorkerPool(const WorkerPool &) = delete;
      WorkerPool &operator=(const WorkerPool &) = delete;

      /**
       * @brief 启动工作线程池
       * @return 启动成功返回 true，否则返回 false
       */
      bool Start();

      /**
       * @brief 停止工作线程池
       */
      void Stop();

      /**
       * @brief 提交事件到工作线程池（阻塞模式）
       * @param event 要处理的网络事件
       *
       * 使用轮询策略将事件分配到不同的 Worker。
       * 此方法可以从任何线程调用，包括 Reactor 线程。
       * 如果队列满，会阻塞直到有空间可用。
       */
      bool SubmitEvent(const NetworkEvent &event);

      /**
       * @brief 尝试提交事件到工作线程池（非阻塞模式）
       * @param event 要处理的网络事件
       * @return 提交成功返回 true，队列满返回 false
       *
       * 使用轮询策略将事件分配到不同的 Worker。
       * 此方法可以从任何线程调用，包括 Reactor 线程。
       * 如果队列满，立即返回 false（用于背压控制）。
       */
      bool TrySubmitEvent(const NetworkEvent &event);

      /**
       * @brief 设置事件回调函数
       * @param callback 当网络事件发生时调用的函数
       *
       * Worker 线程处理完事件后会调用此回调函数。
       */
      void SetEventCallback(EventCallback callback);

      /**
       * @brief 获取所有队列的总大小
       * @return 所有队列中的事件总数
       */
      size_t GetTotalQueueSize() const;

      uint64_t GetSubmitFailures() const;

    private:
      /**
       * @brief Worker 线程的主循环
       * @param worker_id Worker ID（用于日志和调试）
       *
       * 每个 Worker 在独立线程中持续运行，从队列中取出事件并处理。
       */
      void WorkerLoop(int worker_id);

      /**
       * @brief 使用轮询策略选择下一个 Worker
       * @return 选中 Worker 的索引
       */
      size_t SelectNextWorker();

    private:
      size_t worker_count_;                                                      ///< 工作线程数量
      size_t max_queue_size_;                                                    ///< 每个队列的最大容量（0 = 无限制）
      std::vector<std::thread> worker_threads_;                                  ///< 工作线程列表
      std::vector<std::unique_ptr<ConcurrentQueue<NetworkEvent>>> event_queues_; ///< 每个线程的事件队列
      EventCallback event_callback_;                                             ///< 事件回调函数

      std::atomic<size_t> next_worker_index_{0}; ///< 下一个要分配的 Worker 索引（轮询）
      std::atomic<bool> is_running_{false};      ///< Worker Pool 运行状态
      std::atomic<uint64_t> submit_failures_{0}; ///< 提交失败次数（队列满/已停止）
    };

  } // namespace network
} // namespace darwincore

#endif // DARWINCORE_NETWORK_WORKER_POOL_H
