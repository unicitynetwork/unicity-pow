// Copyright (c) 2025 The Unicity Foundation
// Distributed under the MIT software license

#include "util/threadpool.hpp"
#include "util/logging.hpp"
#include <algorithm>

namespace unicity {
namespace util {

ThreadPool::ThreadPool(size_t num_threads, size_t max_queue_size)
    : stop_(false), max_queue_size_(max_queue_size) {
  // If num_threads is 0, use hardware concurrency
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) {
      num_threads = 4; // Fallback if hardware_concurrency() fails
    }
  }

  // Create worker threads
  workers_.reserve(num_threads);
  for (size_t i = 0; i < num_threads; ++i) {
    workers_.emplace_back([this, i] {
      while (true) {
        std::function<void()> task;

        {
          std::unique_lock<std::mutex> lock(this->queue_mutex_);
          this->condition_.wait(lock, [this] {
            return this->stop_.load(std::memory_order_acquire) ||
                   !this->tasks_.empty();
          });

          if (this->stop_.load(std::memory_order_acquire) &&
              this->tasks_.empty()) {
            return;
          }

          task = std::move(this->tasks_.front());
          this->tasks_.pop();
        }

        // Execute task with exception handling
        // Prevents uncaught exceptions from terminating the worker thread
        try {
          task();
          this->tasks_completed_.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception &e) {
          this->task_exceptions_.fetch_add(1, std::memory_order_relaxed);
          LOG_CHAIN_ERROR("ThreadPool worker {} caught exception: {}", i, e.what());
          // Exception is already stored in the packaged_task's future
          // so it will propagate to the caller via future.get()
        } catch (...) {
          this->task_exceptions_.fetch_add(1, std::memory_order_relaxed);
          LOG_CHAIN_ERROR("ThreadPool worker {} caught unknown exception", i);
          // Unknown exception - can't propagate details, but thread survives
        }
      }
    });
  }
}

ThreadPool::~ThreadPool() {
  shutdown();
  wait_for_completion();
}

void ThreadPool::shutdown() {
  // Use atomic store with release semantics
  // This ensures all previous writes are visible to threads that load stop_
  stop_.store(true, std::memory_order_release);
  condition_.notify_all();
}

void ThreadPool::wait_for_completion() {
  // Wait for all worker threads to finish
  for (std::thread &worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

} // namespace util
} // namespace unicity
