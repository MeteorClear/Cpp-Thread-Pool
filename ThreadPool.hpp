#pragma once

#ifndef CPP_MT_THREAD_POOL_HPP
#define CPP_MT_THREAD_POOL_HPP

// Core utilities
#include <cassert>
#include <type_traits>
#include <memory>
#include <functional>
#include <algorithm>

// Containers
#include <vector>
#include <queue>

// Concurrency
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <future>

// I/O
#include <stdexcept>
#include <iostream>


// Check C++ version
#if defined(_MSVC_LANG)
#define _THREADPOOL_CPP_VERSION _MSVC_LANG
#else
#define _THREADPOOL_CPP_VERSION __cplusplus
#endif

namespace mt {  // multi-threading

    class ThreadPool {
    public:
        using Work = std::function<void()>;
        using WorkPtr = std::shared_ptr<Work>;
        //using Task = std::pair<int, Work>;  // pair: {priority, work_function}
        using Task = std::pair<int, WorkPtr>;  // pair: {priority, shared_ptr_to_work}

        template <typename F, typename... Args>
#if _THREADPOOL_CPP_VERSION >= 201703L
        using return_type_t = typename std::invoke_result<F, Args...>::type;
#else
        using return_type_t = typename std::result_of<F(Args...)>::type;
#endif

        static constexpr int DEFAULT_PRIORITY = 0;

        // Constructor
        explicit ThreadPool(size_t threadCount = 0) {
            size_t hw = std::thread::hardware_concurrency();
            threadCount_ = (threadCount == 0 || threadCount > hw) ? hw : threadCount;
            threadCount_ = std::max(threadCount_, size_t{ 1 });

            threads_.reserve(threadCount_);

            // Start worker threads
            for (size_t i = 0; i < threadCount_; ++i) {
                threads_.emplace_back(&ThreadPool::workerLoop, this);
            }

            assert(threads_.size() == threadCount_ && "Thread vector size mismatch.");
            assert(runningTasks_.load() == 0 && "Initial running tasks must be 0.");
        }
        // Destructor
        ~ThreadPool() noexcept {
            this->shutdown_internal(true);
        }

        // -------------------------------- Methods --------------------------------

        // Enqueue future return task with priority
        template<class F, class... Args>
        auto enqueue(int priority, F&& f, Args&&... args)
            -> std::future<return_type_t<F, Args...>>
        {
            using ResultType = return_type_t<F, Args...>;

            auto task = std::make_shared<std::packaged_task<ResultType()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...)
            );

            std::future<ResultType> res = task->get_future();

            //Work workWrapper = [task]() { (*task)(); };
            auto workWrapper = std::make_shared<Work>([task]() {
                (*task)();
                });

            // Enqueue wrapped task into task queue
            this->queueTask(priority, std::move(workWrapper));

            return res;
        }

        // Enqueue future return task
        template<class F, class... Args>
        auto enqueue(F&& f, Args&&... args)
            -> std::future<return_type_t<F, Args...>>
        {
            return this->enqueue(DEFAULT_PRIORITY, std::forward<F>(f), std::forward<Args>(args)...);
        }

        // Wait until the queue is empty and all running tasks are complete
        // Throw exception if there is any work left in the task queue while pause state
        void wait() {
            this->checkDeadlock("wait");

            std::unique_lock<std::mutex> lock(queueMutex_);

            waitCV_.wait(lock, [this]() {
                bool isDone = taskQueue_.empty() && (runningTasks_.load(std::memory_order_acquire) == 0);
                bool isBlockedByPause = !taskQueue_.empty() && pauseFlag_.load(std::memory_order_acquire);
                return isDone || isBlockedByPause;
                });

            if (pauseFlag_.load(std::memory_order_acquire) && !taskQueue_.empty()) {
                throw std::runtime_error("ThreadPool is paused with pending tasks.");
            }

            assert(taskQueue_.empty() && "Wait finished but queue is not empty.");
            assert(runningTasks_.load(std::memory_order_acquire) == 0 && "Wait finished but tasks are running.");
        }

        // Pause the working of tasks
        // Executed Tasks prior to the call are not interrupted
        void pause() {
            pauseFlag_.store(true, std::memory_order_release);
            waitCV_.notify_all();
        }

        // Resume the working of tasks
        void resume() {
            pauseFlag_.store(false, std::memory_order_release);
            queueCV_.notify_all();
        }

        // Clear all waiting tasks from the queue
        // Any associated std::future will receive std::future_error (broken_promise)
        // Recommended to use after call pause()
        void clearQueue() {
            std::unique_lock<std::mutex> lock(queueMutex_);
            std::priority_queue<Task, std::vector<Task>, TaskCompare> empty_queue;
            taskQueue_.swap(empty_queue);

            // If no tasks are running, notify wait calls that the pool is idle
            if (runningTasks_.load(std::memory_order_acquire) == 0) {
                waitCV_.notify_all();
            }
        }

        // Shut down the thread pool and destroy worker threads (Graceful)
        // New thread pool must be used to enable thread again after calling this method
        void shutdown() {
            this->checkDeadlock("shutdown");
            shutdown_internal(false);
        }

        // Shut down the thread pool and destroy worker threads
        // Remove remained tasks and terminate
        void terminate() {
            this->checkDeadlock("terminate");
            shutdown_internal(true);
        }

        // ----------------------------- Status & Stats -----------------------------

        // Return the number of waiting task in queue
        size_t getQueueSize() const {
            std::unique_lock<std::mutex> lock(queueMutex_);
            return taskQueue_.size();
        }

        // Return the number of active threads
        size_t getThreadCount() const { return threadCount_; }

        // Return the number of running tasks
        int getRunningTasks() const { return runningTasks_.load(std::memory_order_acquire); }

        // Return the status of pause flag
        // true: paused
        // false: working
        bool isPaused() const { return pauseFlag_.load(std::memory_order_acquire); }

        // Return the status of stop flag
        // true: stopped
        // false: active
        bool isStopped() const {
            return stopFlag_.load(std::memory_order_acquire);
        }

    private:

        // Comparator for the priority queue, compares only the first element(int priority)
        struct TaskCompare {
            bool operator()(const Task& lhs, const Task& rhs) const {
                return lhs.first < rhs.first;
            }
        };

        // Enqueue the task with specific priority into task queue
        inline void queueTask(int priority, WorkPtr work_wrapper) {
            assert(work_wrapper && "Attempted to enqueue an empty task.");

            {   // Lock
                std::unique_lock<std::mutex> lock(queueMutex_);

                // shutdown status
                if (stopFlag_.load(std::memory_order_acquire)) {
                    throw std::runtime_error("Cannot enqueue task: ThreadPool is shut down.");
                }

                taskQueue_.emplace(priority, std::move(work_wrapper));
            }   // Unlock

            // Notify one waiting worker
            if (!pauseFlag_.load(std::memory_order_acquire)) {
                queueCV_.notify_one();
            }
        }

        // Check deadlock by checking if calling self thread
        void checkDeadlock(const char* callerName) {
            const std::thread::id this_id = std::this_thread::get_id();
            bool is_worker = false;

            for (const auto& t : threads_) {
                if (t.get_id() == this_id) {
                    is_worker = true;
                    break;
                }
            }

            if (is_worker) {
                std::string msg = std::string("ThreadPool::") + callerName +
                    "() cannot be called from a worker thread. This causes a deadlock.";
#ifndef NDEBUG
                std::cerr << "[ThreadPool]ERROR:Assertion failed: " << msg << std::endl;
                assert(false && "Deadlock detected");
#else
                throw std::logic_error(msg);
#endif
            }
        }

        // Shut down the thread pool and destroy worker threads
        // If immediate is true, clear task queue 
        void shutdown_internal(bool immediate) {
            {   // Lock
                std::unique_lock<std::mutex> lock(queueMutex_);

                if (stopFlag_.load(std::memory_order_acquire)) return;

                stopFlag_.store(true, std::memory_order_release);
                pauseFlag_.store(false, std::memory_order_release);

                if (immediate) {  // Clear task queue
                    std::priority_queue<Task, std::vector<Task>, TaskCompare> empty;
                    std::swap(taskQueue_, empty);
                }
            }   // Unlock

            queueCV_.notify_all();
            waitCV_.notify_all();

            auto this_id = std::this_thread::get_id();

            for (auto& t : threads_) {
                if (t.joinable()) {
                    if (t.get_id() == this_id) {  // Joining self thread cause deadlock
                        t.detach();
                    }
                    else {
                        t.join();
                    }
                }
            }
            threads_.clear();
        }

        // Main loop for each worker thread
        void workerLoop() {
            while (true) {
                //Work work;
                WorkPtr work_ptr;

                {   // Lock
                    std::unique_lock<std::mutex> lock(queueMutex_);

                    // Wait condition
                    queueCV_.wait(lock, [&]() {
                        return stopFlag_.load(std::memory_order_acquire) || (!pauseFlag_.load(std::memory_order_acquire) && !taskQueue_.empty());
                        });

                    // Exit condition
                    if (stopFlag_.load(std::memory_order_acquire) && taskQueue_.empty()) { return; }

                    // Pause condition
                    if (pauseFlag_.load(std::memory_order_acquire)) { continue; }

                    if (!taskQueue_.empty()) {
                        //work = std::move(taskQueue_.top().second);  // std::move const warning, taskQueue_.top().second is const Work& type
                        //work = taskQueue_.top().second;  // copy is heavy
                        //work = std::move(const_cast<Work&>(taskQueue_.top().second));  // const_cast risk
                        work_ptr = taskQueue_.top().second;
                        taskQueue_.pop();

                        //assert(work && "Popped empty task from queue.");
                        assert(work_ptr && "Popped empty task from queue.");

                        runningTasks_.fetch_add(1, std::memory_order_release);
                    }
                    else { continue; }
                }   // Unlock

                // Execute the task
                try {
                    //work();
                    if (work_ptr) {
                        (*work_ptr)();
                    }
                }
                catch (const std::exception& e) {
                    std::cerr << "[ThreadPool]ERROR:Worker thread caught exception: " << e.what() << '\n';
                }
                catch (...) {
                    std::cerr << "[ThreadPool]ERROR:Worker thread caught unknown exception." << '\n';
                }

                {   // Lock
                    std::unique_lock<std::mutex> lock(queueMutex_);

                    int prevCount = runningTasks_.fetch_sub(1, std::memory_order_release);
                    assert(prevCount > 0 && "Running tasks count underflow");

                    // Decrement running task count
                    // If this was the last running task, check if we need to notify wait
                    if (prevCount == 1) {
                        if (taskQueue_.empty()) {
                            waitCV_.notify_all();  // Notify waiting threads (pool is idle)
                        }
                    }
                }   // Unlock
            }   // End of while
        }

    private:
        std::atomic<bool> stopFlag_{ false };       // Signals workers to terminate permanently
        std::atomic<bool> pauseFlag_{ false };      // Signals workers to temporarily stop consuming tasks
        std::atomic<int> runningTasks_{ 0 };        // Counter for currently executing tasks

        size_t threadCount_ = 0;                    // Target number of worker threads
        std::vector<std::thread> threads_;          // Container for worker threads

        // Priority queue of tasks, sort by priority
        std::priority_queue<Task, std::vector<Task>, TaskCompare> taskQueue_;

        mutable std::mutex queueMutex_;             // Mutex to protect the task queue
        std::condition_variable queueCV_;           // CV to signal workers when a new task arrives (or shutdown)
        std::condition_variable waitCV_;            // CV to signal threads waiting on wait() when the pool becomes idle
    };  // class end

}  // multi-threading

#endif  // CPP_MT_THREAD_POOL_HPP
