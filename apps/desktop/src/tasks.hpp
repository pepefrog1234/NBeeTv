// 執行緒之間交接工作用的兩樣東西，對應 macOS 版的 DispatchQueue：
//   MainQueue  別的執行緒把「回到主執行緒要做的事」放進來，主迴圈每一圈取出來做（DispatchQueue.main.async）。
//   Worker     一條自己的執行緒，照放進來的順序一件一件做（序列佇列）。
// 介面的狀態（App）只在主執行緒上讀寫；花時間的事（解調、影像編碼、PTT、縮圖）各自丟到一個 Worker。
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace nbtvgui {

/// 程式啟動後經過的秒數（不受系統時鐘調整影響）。
inline double uptime_seconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

class MainQueue {
public:
    static MainQueue& shared() {
        static MainQueue queue;
        return queue;
    }

    void post(std::function<void()> job) {
        std::function<void()> waker;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
            waker = waker_;
        }
        if (waker) waker();
    }

    /// 主執行緒呼叫：把目前排著的工作做完。做的過程中新放進來的留到下一次。
    void drain() {
        std::deque<std::function<void()>> jobs;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs.swap(jobs_);
        }
        for (auto& job : jobs) job();
    }

    /// 有新工作時用它叫醒正在等事件的主迴圈。
    void set_waker(std::function<void()> waker) {
        std::lock_guard<std::mutex> lock(mutex_);
        waker_ = std::move(waker);
    }

private:
    std::mutex mutex_;
    std::deque<std::function<void()>> jobs_;
    std::function<void()> waker_;
};

inline void on_main(std::function<void()> job) { MainQueue::shared().post(std::move(job)); }

class Worker {
public:
    Worker() : thread_([this] { run(); }) {}
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// 做完已經排進來的工作才結束。
    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        wake_.notify_one();
    }

    /// 排進去並等它做完（前面排著的也都做完了）。不可以在這條 Worker 自己的執行緒上呼叫。
    void sync(std::function<void()> job) {
        std::promise<void> done;
        std::future<void> finished = done.get_future();
        post([&job, &done] {
            job();
            done.set_value();
        });
        finished.wait();
    }

private:
    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace nbtvgui
