#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace unicity {
namespace util {

/**
 * Thread-safe thread pool for parallel task execution
 *
 * Features:
 * - Automatic thread count based on hardware concurrency
 * - Exception-safe worker threads (exceptions don't kill threads)
 * - Graceful shutdown with pending task completion
 * - Task monitoring and statistics
 * - Optional queue size limit to prevent memory exhaustion
 *
 * Usage:
 *   ThreadPool pool(4);  // 4 worker threads
 *   auto future = pool.enqueue([](){ return 42; });
 *   int result = future.get();
 *   pool.shutdown();  // Stop accepting new tasks
 *   pool.wait_for_completion();  // Wait for pending tasks
 */
class ThreadPool {
public:
  /**
   * Constructor - create pool with specified number of threads
   * @param num_threads Number of worker threads (0 = use hardware concurrency)
   * @param max_queue_size Maximum queued tasks (0 = unlimited)
   */
  explicit ThreadPool(size_t num_threads = 0, size_t max_queue_size = 0);

  /**
   * Destructor - stops accepting new tasks and waits for all tasks to complete
   */
  ~ThreadPool();

  // Explicitly delete copy/move (thread pool is not copyable/movable)
  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  /**
   * Enqueue a task for execution
   * Returns a future that will contain the result
   * @throws std::runtime_error if pool is stopped or queue is full
   */
  template <class F, class... Args>
  auto enqueue(F &&f, Args &&...args)
      -> std::future<typename std::invoke_result<F, Args...>::type>;

  /**
   * Stop accepting new tasks (pending tasks will still execute)
   * Safe to call multiple times
   */
  void shutdown();

  /**
   * Wait for all pending tasks to complete
   * Should be called after shutdown() for graceful termination
   */
  void wait_for_completion();

  /**
   * Get number of worker threads
   */
  size_t size() const { return workers_.size(); }

  /**
   * Get number of pending tasks in queue
   * Thread-safe
   */
  size_t pending_tasks() const {
    std::unique_lock<std::mutex> lock(queue_mutex_);
    return tasks_.size();
  }

  /**
   * Check if pool is stopped (not accepting new tasks)
   */
  bool is_stopped() const {
    return stop_.load(std::memory_order_acquire);
  }

  /**
   * Get total number of tasks completed (for monitoring/debugging)
   */
  size_t tasks_completed() const {
    return tasks_completed_.load(std::memory_order_relaxed);
  }

  /**
   * Get total number of task exceptions (for monitoring/debugging)
   */
  size_t task_exceptions() const {
    return task_exceptions_.load(std::memory_order_relaxed);
  }

private:
  // Worker threads
  std::vector<std::thread> workers_;

  // Task queue
  std::queue<std::function<void()>> tasks_;
  size_t max_queue_size_;  // 0 = unlimited

  // Synchronization
  mutable std::mutex queue_mutex_;
  std::condition_variable condition_;
  std::atomic<bool> stop_;  // Atomic to prevent initialization race

  // Statistics (atomic for thread-safe access)
  std::atomic<size_t> tasks_completed_{0};
  std::atomic<size_t> task_exceptions_{0};
};

// Implementation of enqueue (must be in header for template)
template <class F, class... Args>
auto ThreadPool::enqueue(F &&f, Args &&...args)
    -> std::future<typename std::invoke_result<F, Args...>::type> {
  using return_type = typename std::invoke_result<F, Args...>::type;

  auto task = std::make_shared<std::packaged_task<return_type()>>(
      std::bind(std::forward<F>(f), std::forward<Args>(args)...));

  std::future<return_type> res = task->get_future();
  {
    std::unique_lock<std::mutex> lock(queue_mutex_);

    // Don't allow enqueueing after stopping the pool
    if (stop_.load(std::memory_order_acquire))
      throw std::runtime_error("enqueue on stopped ThreadPool");

    // Check queue size limit
    if (max_queue_size_ > 0 && tasks_.size() >= max_queue_size_)
      throw std::runtime_error("ThreadPool queue full");

    tasks_.emplace([task]() { (*task)(); });
  }
  condition_.notify_one();
  return res;
}

} // namespace util
} // namespace unicity
