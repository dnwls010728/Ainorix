#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rpg {

// Threads that take tasks from one bounded queue. With one thread it is a serial executor (the
// store thread: the only one that touches SQLite); with several it is the pool that hashes
// passwords. The I/O thread never waits for it: a full queue refuses the task and the caller
// answers "busy". Results go back with FrameServer::Post.
class TaskQueue {
public:
    TaskQueue(size_t threads, size_t capacity) : capacity_(capacity) {
        for (size_t i = 0; i < (threads ? threads : 1); ++i) threads_.emplace_back([this] { Work(); });
    }
    ~TaskQueue() { Stop(); }
    TaskQueue(const TaskQueue&) = delete;
    TaskQueue& operator=(const TaskQueue&) = delete;

    // False when the queue is full or stopped.
    bool Push(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_ || tasks_.size() >= capacity_) return false;
            tasks_.push_back(std::move(task));
        }
        ready_.notify_one();
        return true;
    }
    // Runs what is queued, then joins. Safe to call twice.
    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
        }
        ready_.notify_all();
        for (std::thread& thread : threads_)
            if (thread.joinable()) thread.join();
    }
    size_t Pending() {
        std::lock_guard<std::mutex> lock(mutex_);
        return tasks_.size();
    }
    size_t Threads() const { return threads_.size(); }

private:
    void Work() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
                if (tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::function<void()>> tasks_;
    std::vector<std::thread> threads_;
    size_t capacity_;
    bool stopped_ = false;
};

}  // namespace rpg
