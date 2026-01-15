//
// DarwinCore Network 模块
// Worker Pool 实现
//
// 功能说明：
//   实现工作线程池，用于处理网络事件和执行业务逻辑回调。
//
// 作者: DarwinCore Network 团队
// 日期: 2026

#include <algorithm>
#include <chrono>

#include "worker_pool.h"
#include <darwincore/network/configuration.h>
#include <darwincore/network/logger.h>

namespace darwincore {
namespace network {

WorkerPool::WorkerPool(size_t worker_count)
    : worker_count_(worker_count), is_running_(false) {
  if (worker_count_ == 0) {
    worker_count_ = SocketConfiguration::kDefaultWorkerCount;
  }

  event_queues_.resize(worker_count_);
  for (auto &queue : event_queues_) {
    queue = std::make_unique<ConcurrentQueue<NetworkEvent>>();
  }
}

WorkerPool::~WorkerPool() { Stop(); }

bool WorkerPool::Start() {
  if (is_running_) {
    return false;
  }

  is_running_ = true;
  for (size_t i = 0; i < worker_count_; ++i) {
    worker_threads_.emplace_back(&WorkerPool::WorkerLoop, this, i);
  }

  return true;
}

void WorkerPool::Stop() {
  if (!is_running_) {
    return;
  }

  is_running_ = false;

  // 通知所有队列停止，唤醒等待中的 Worker 线程
  for (auto &queue : event_queues_) {
    queue->NotifyStop();
  }

  for (auto &worker_thread : worker_threads_) {
    if (worker_thread.joinable()) {
      worker_thread.join();
    }
  }

  worker_threads_.clear();
}

void WorkerPool::SubmitEvent(const NetworkEvent &event) {
  size_t worker_id = event.connection_id % worker_count_;
  NW_LOG_TRACE("[WorkerPool::SubmitEvent] type="
               << static_cast<int>(event.type) << ", conn_id="
               << event.connection_id << ", worker_id=" << worker_id
               << ", worker_count_=" << worker_count_);
  event_queues_[worker_id]->Enqueue(event);
}

void WorkerPool::SetEventCallback(EventCallback callback) {
  NW_LOG_DEBUG("[WorkerPool::SetEventCallback] 设置回调，callback="
               << (callback != nullptr));
  event_callback_ = std::move(callback);
}

void WorkerPool::WorkerLoop(int worker_id) {
  auto &queue = event_queues_[worker_id];
  NW_LOG_DEBUG("[WorkerPool] Worker " << worker_id << " 启动");

  while (is_running_) {
    NetworkEvent event(NetworkEventType::kData, 0);

    // 使用阻塞等待（100ms超时），替代 TryDequeue + sleep_for
    // 这样既能快速响应事件，又能定期检查 is_running_ 状态
    if (queue->WaitDequeue(event, std::chrono::milliseconds(100))) {
      NW_LOG_TRACE("[WorkerPool] Worker "
                   << worker_id
                   << " 处理事件: type=" << static_cast<int>(event.type)
                   << ", conn_id=" << event.connection_id
                   << ", payload_size=" << event.payload.size()
                   << ", event_callback_=" << (event_callback_ != nullptr));
      if (event_callback_) {
        NW_LOG_TRACE("[WorkerPool] 调用 event_callback_");
        event_callback_(event);
        NW_LOG_TRACE("[WorkerPool] event_callback_ 返回");
      }
    }
    // WaitDequeue 超时或被 NotifyStop 唤醒时，继续检查 is_running_
  }

  NetworkEvent remaining_event(NetworkEventType::kData, 0);
  while (queue->TryDequeue(remaining_event)) {
    if (event_callback_) {
      event_callback_(remaining_event);
    }
  }
}

} // namespace network
} // namespace darwincore
