//
// DarwinCore Network 模块
// Worker Pool 实现
//
// 功能说明：
//   实现工作线程池，用于处理网络事件和执行业务逻辑回调。
//   支持背压控制和非阻塞事件提交。
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <algorithm>
#include <chrono>

#include "worker_pool.h"
#include <darwincore/network/configuration.h>
#include <darwincore/network/logger.h>
#include <pthread.h>

namespace darwincore
{
  namespace network
  {

    WorkerPool::WorkerPool(size_t worker_count, size_t max_queue_size)
        : worker_count_(worker_count), max_queue_size_(max_queue_size),
          is_running_(false)
    {
      if (worker_count_ == 0)
      {
        worker_count_ = SocketConfiguration::kDefaultWorkerCount;
      }

      event_queues_.resize(worker_count_);
      for (auto &queue : event_queues_)
      {
        queue = std::make_unique<ConcurrentQueue<NetworkEvent>>(max_queue_size_);
      }

      NW_LOG_DEBUG("[WorkerPool] 构造: worker_count=" << worker_count_
                                                      << ", max_queue_size=" << max_queue_size_);
    }

    WorkerPool::~WorkerPool() { Stop(); }

    bool WorkerPool::Start()
    {
      if (is_running_)
      {
        return false;
      }

      is_running_ = true;
      for (size_t i = 0; i < worker_count_; ++i)
      {
        worker_threads_.emplace_back(&WorkerPool::WorkerLoop, this, i);
      }

      NW_LOG_INFO("[WorkerPool] 启动: " << worker_count_ << " 个工作线程");
      return true;
    }

    void WorkerPool::Stop()
    {
      if (!is_running_)
      {
        return;
      }

      is_running_ = false;

      // 通知所有队列停止，唤醒等待中的 Worker 线程
      for (auto &queue : event_queues_)
      {
        queue->NotifyStop();
      }

      for (auto &worker_thread : worker_threads_)
      {
        if (worker_thread.joinable())
        {
          worker_thread.join();
        }
      }

      worker_threads_.clear();

      NW_LOG_INFO("[WorkerPool] 停止");
    }

    void WorkerPool::SubmitEvent(const NetworkEvent &event)
    {
      size_t worker_id = event.connection_id % worker_count_;

      // 阻塞模式：如果队列满，等待直到有空间
      while (is_running_)
      {
        if (event_queues_[worker_id]->TryEnqueue(event))
        {
          break;
        }
        // 队列满，短暂等待后重试
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }

      NW_LOG_TRACE("[WorkerPool::SubmitEvent] type="
                   << static_cast<int>(event.type) << ", conn_id="
                   << event.connection_id << ", worker_id=" << worker_id);
    }

    bool WorkerPool::TrySubmitEvent(const NetworkEvent &event)
    {
      size_t worker_id = event.connection_id % worker_count_;
      bool success = event_queues_[worker_id]->TryEnqueue(event);

      if (!success)
      {
        NW_LOG_WARNING("[WorkerPool::TrySubmitEvent] 队列满: type="
                       << static_cast<int>(event.type) << ", conn_id="
                       << event.connection_id << ", worker_id=" << worker_id);
      }
      else
      {
        NW_LOG_TRACE("[WorkerPool::TrySubmitEvent] type="
                     << static_cast<int>(event.type) << ", conn_id="
                     << event.connection_id << ", worker_id=" << worker_id);
      }

      return success;
    }

    void WorkerPool::SetEventCallback(EventCallback callback)
    {
      NW_LOG_DEBUG("[WorkerPool::SetEventCallback] 设置回调，callback="
                   << (callback != nullptr));
      event_callback_ = std::move(callback);
    }

    size_t WorkerPool::GetTotalQueueSize() const
    {
      size_t total = 0;
      for (const auto &queue : event_queues_)
      {
        total += queue->Size();
      }
      return total;
    }

    void WorkerPool::WorkerLoop(int worker_id)
    {
      pthread_setname_np(("darwincore.network.worker." + std::to_string(worker_id)).c_str());

      auto &queue = event_queues_[worker_id];
      NW_LOG_DEBUG("[WorkerPool] Worker " << worker_id << " 启动");

      while (is_running_)
      {
        NetworkEvent event(NetworkEventType::kData, 0);

        // 使用阻塞等待（100ms超时），替代 TryDequeue + sleep_for
        // 这样既能快速响应事件，又能定期检查 is_running_ 状态
        if (queue->WaitDequeue(event, std::chrono::milliseconds(100)))
        {
          NW_LOG_TRACE("[WorkerPool] Worker "
                       << worker_id
                       << " 处理事件: type=" << static_cast<int>(event.type)
                       << ", conn_id=" << event.connection_id
                       << ", payload_size=" << event.payload.size()
                       << ", event_callback_=" << (event_callback_ != nullptr));
          if (event_callback_)
          {
            NW_LOG_TRACE("[WorkerPool] 调用 event_callback_");
            event_callback_(event);
            NW_LOG_TRACE("[WorkerPool] event_callback_ 返回");
          }
        }
        // WaitDequeue 超时或被 NotifyStop 唤醒时，继续检查 is_running_
      }

      // 处理剩余事件
      NetworkEvent remaining_event(NetworkEventType::kData, 0);
      while (queue->TryDequeue(remaining_event))
      {
        if (event_callback_)
        {
          event_callback_(remaining_event);
        }
      }

      NW_LOG_DEBUG("[WorkerPool] Worker " << worker_id << " 退出");
    }

  } // namespace network
} // namespace darwincore
