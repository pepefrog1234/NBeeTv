#include "parallel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

namespace nbtv {

namespace {

// 一次 parallel_for：放在呼叫者的堆疊上，幫手只在它還排在 Pool 裡、或自己正在做的時候碰它。
struct Job {
    void (*fn)(void*, int, int) = nullptr;
    void* context = nullptr;
    long long count = 0;
    long long grain = 1;
    std::atomic<long long> next{0};  // 下一段從哪裡開始；誰先加到就是誰的

    // 以下由 Pool 的鎖保護
    int limit = 0;    // 最多讓幾條幫手來做
    int joined = 0;   // 來過幾條
    int helpers = 0;  // 現在還有幾條在做

    // 一段一段領來做，領完為止。fn 丟出例外的話直接結束程式（別的執行緒還握著這個 Job，沒辦法往外傳）。
    void work() noexcept {
        for (;;) {
            const long long begin = next.fetch_add(grain, std::memory_order_relaxed);
            if (begin >= count) return;
            fn(context, static_cast<int>(begin), static_cast<int>(std::min(count, begin + grain)));
        }
    }
    bool taken() const { return next.load(std::memory_order_relaxed) >= count; }
};

// 這條執行緒正在做某一件工作的一段（或者它就是幫手）：這時候再呼叫 parallel_for 就在原地做
thread_local bool inside = false;

class Pool {
public:
    void run(Job& job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            spawn_locked(job.limit);
            jobs_.push_back(&job);
        }
        wake_.notify_all();
        job.work();
        std::unique_lock<std::mutex> lock(mutex_);
        drop_locked(&job);  // 之後不會再有幫手加進來
        done_.wait(lock, [&job] { return job.helpers == 0; });
    }

private:
    void spawn_locked(int wanted) {
        while (threads_ < wanted && !refused_) {
            try {
                std::thread([this] { loop(); }).detach();
                ++threads_;
            } catch (const std::system_error&) {
                refused_ = true;  // 系統不給開：有幾條用幾條，不再試
            }
        }
    }

    void loop() {
        inside = true;
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            Job* job = nullptr;
            wake_.wait(lock, [this, &job] { return (job = pick_locked()) != nullptr; });
            ++job->joined;
            ++job->helpers;
            lock.unlock();
            job->work();
            lock.lock();
            drop_locked(job);
            if (--job->helpers == 0) done_.notify_all();
        }
    }

    // 排在最前面、還有段落沒被領走、幫手也還沒滿的那一件
    Job* pick_locked() {
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            Job* job = *it;
            if (job->taken()) {
                it = jobs_.erase(it);
                continue;
            }
            if (job->joined < job->limit) return job;
            ++it;
        }
        return nullptr;
    }

    void drop_locked(Job* job) {
        const auto it = std::find(jobs_.begin(), jobs_.end(), job);
        if (it != jobs_.end()) jobs_.erase(it);
    }

    std::mutex mutex_;
    std::condition_variable wake_;  // 有新的工作
    std::condition_variable done_;  // 某一件工作的幫手都做完了
    std::deque<Job*> jobs_;
    int threads_ = 0;
    bool refused_ = false;
};

Pool& pool() {
    static Pool* const shared = new Pool;  // 故意不收回，見 parallel.hpp
    return *shared;
}

int default_threads() {
    if (const char* text = std::getenv("NBTV_THREADS"); text != nullptr && *text != '\0') {
        char* end = nullptr;
        const long wanted = std::strtol(text, &end, 10);
        if (end != text && *end == '\0' && wanted >= 1) return static_cast<int>(std::min<long>(wanted, kMaxThreads));
    }
    const unsigned processors = std::thread::hardware_concurrency();  // 問不到時是 0
    return std::clamp(static_cast<int>(std::min<unsigned>(processors, kMaxThreads)), 1, kDefaultMaxThreads);
}

std::atomic<int> thread_setting{0};  // 0：還沒決定

}  // namespace

int parallel_threads() {
    int threads = thread_setting.load(std::memory_order_relaxed);
    if (threads == 0) {
        threads = default_threads();
        thread_setting.store(threads, std::memory_order_relaxed);  // 兩條執行緒同時算到這裡，算出來的值也一樣
    }
    return threads;
}

void set_parallel_threads(int threads) {
    thread_setting.store(threads <= 0 ? default_threads() : std::min(threads, kMaxThreads), std::memory_order_relaxed);
}

void detail::parallel_run(int count, int grain, void (*fn)(void*, int, int), void* context) {
    if (count <= 0) return;
    if (grain < 1) grain = 1;
    const int threads = parallel_threads();
    if (inside || threads <= 1 || count <= grain) {
        // 照同樣的切法依序做：fn 看到的每一段和分給幾條執行緒時一模一樣
        for (long long begin = 0; begin < count; begin += grain)
            fn(context, static_cast<int>(begin), static_cast<int>(std::min<long long>(count, begin + grain)));
        return;
    }
    Job job;
    job.fn = fn;
    job.context = context;
    job.count = count;
    job.grain = grain;
    job.limit = threads - 1;
    inside = true;
    pool().run(job);
    inside = false;
}

}  // namespace nbtv
