//
// DarwinCore Network Module
// Thread-Safe Queue for Events
//
// Description:
//   A thread-safe queue implementation using mutex and condition variable.
//   Used by WorkerPool to receive NetworkEvents from Reactors.
//
// Thread Safety:
//   - All operations are thread-safe
//   - Multiple threads can enqueue and dequeue concurrently
//
// Author: DarwinCore Network Team
// Date: 2026

#ifndef DARWINCORE_NETWORK_CONCURRENT_QUEUE_H
#define DARWINCORE_NETWORK_CONCURRENT_QUEUE_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>

namespace darwincore {
namespace network {

/**
 * @brief Thread-safe queue for inter-thread communication
 *
 * This template class provides a thread-safe queue that can be used
 * to pass data between threads. It uses a mutex to protect access
 * and a condition variable to notify waiting threads.
 *
 * Usage Example:
 *   @code
 *   ConcurrentQueue<int> queue;
 *   queue.Enqueue(42);  // Thread-safe enqueue
 *
 *   int value;
 *   if (queue.TryDequeue(value)) {
 *     // Successfully dequeued
 *   }
 *   @endcode
 *
 * @tparam T Type of elements stored in the queue
 */
template <typename T> class ConcurrentQueue {
public:
  /// Default constructor
  ConcurrentQueue() : is_stopped_(false) {}

  /// Destructor
  ~ConcurrentQueue() = default;

  // Non-copyable and non-movable
  ConcurrentQueue(const ConcurrentQueue &) = delete;
  ConcurrentQueue &operator=(const ConcurrentQueue &) = delete;

  /**
   * @brief Enqueue an element to the queue
   * @param value Value to enqueue (thread-safe)
   */
  void Enqueue(const T &value) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.push(value);
    }
    condition_variable_.notify_one();
  }

  /**
   * @brief Try to dequeue an element from the queue (non-blocking)
   * @param result Reference to store the dequeued value
   * @return true if an element was dequeued, false if queue is empty
   */
  bool TryDequeue(T &result) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return false;
    }
    result = std::move(queue_.front());
    queue_.pop();
    return true;
  }

  /**
   * @brief Wait and dequeue an element with timeout (blocking)
   * @param result Reference to store the dequeued value
   * @param timeout Maximum time to wait
   * @return true if an element was dequeued, false if timeout or stopped
   *
   * This method blocks until:
   *   - An element is available in the queue
   *   - The timeout expires
   *   - NotifyStop() is called
   */
  bool WaitDequeue(T &result, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);

    bool success = condition_variable_.wait_for(
        lock, timeout, [this] { return !queue_.empty() || is_stopped_; });

    if (!success || is_stopped_ || queue_.empty()) {
      return false;
    }

    result = std::move(queue_.front());
    queue_.pop();
    return true;
  }

  /**
   * @brief Notify all waiting threads to stop
   *
   * Call this method to wake up all threads waiting in WaitDequeue.
   * After calling this, WaitDequeue will return false immediately.
   */
  void NotifyStop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      is_stopped_ = true;
    }
    condition_variable_.notify_all();
  }

  /**
   * @brief Reset the stopped state
   *
   * Call this to allow WaitDequeue to work again after NotifyStop.
   */
  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_stopped_ = false;
  }

  /**
   * @brief Check if the queue is empty
   * @return true if queue is empty, false otherwise
   */
  bool IsEmpty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  /**
   * @brief Check if the queue is stopped
   * @return true if stopped, false otherwise
   */
  bool IsStopped() const { return is_stopped_; }

private:
  mutable std::mutex mutex_; ///< Mutex for thread synchronization
  std::queue<T> queue_;      ///< Underlying queue
  std::condition_variable
      condition_variable_;       ///< Condition variable for notifications
  std::atomic<bool> is_stopped_; ///< Flag to indicate stop notification
};

} // namespace network
} // namespace darwincore

#endif // DARWINCORE_NETWORK_CONCURRENT_QUEUE_H
