#pragma once

#ifndef CPP_MT_THREAD_POOL_HPP
#define CPP_MT_THREAD_POOL_HPP

// Core utilities
#include <cassert>
#include <type_traits>
#include <memory>
#include <functional>
#include <algorithm>
#include <utility>

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

/// @brief Multi-threading utilities namespace.
namespace mt {

    /**
     * @brief A flexible, priority-based ThreadPool implementation.
     *
     * Features:
     * - Priority scheduling (Higher integer = Higher priority).
     * - Task submission returning std::future.
     * - Pause/Resume functionality.
     */
    class ThreadPool {
    public:
        using Work = std::function<void()>;
        using WorkPtr = std::shared_ptr<Work>;
        //using Task = std::pair<int, Work>;  // pair: {priority, work_function}
        using Task = std::pair<int, WorkPtr>;  // pair: {priority, shared_ptr_to_work}

        /// @brief Helper to determine the return type of a callable.
        template <typename F, typename... Args>
#if _THREADPOOL_CPP_VERSION >= 201703L
        using return_type_t = typename std::invoke_result<F, Args...>::type;
#else
        using return_type_t = typename std::result_of<F(Args...)>::type;
#endif

        /// @brief Default priority for tasks if not specified.
        static constexpr int DEFAULT_PRIORITY = 0;

        /**
         * @brief Construct a new Thread Pool.
         *
         * @param threadCount The number of worker threads to spawn.
         *                    If 0, defaults to std::thread::hardware_concurrency().
         */
        explicit inline ThreadPool(size_t threadCount = 0) {
            size_t hw = std::thread::hardware_concurrency();
            this->threadCount_ = (threadCount == 0 || threadCount > hw) ? hw : threadCount;
            this->threadCount_ = std::max(this->threadCount_, size_t{ 1 });

            this->threads_.reserve(this->threadCount_);

            // Start worker threads
            for (size_t i = 0; i < this->threadCount_; ++i) {
                this->threads_.emplace_back(&ThreadPool::workerLoop, this);
            }

            assert(this->threads_.size() == this->threadCount_ && "Thread vector size mismatch.");
            assert(this->runningTasks_.load() == 0 && "Initial running tasks must be 0.");
        }
        
        /**
         * @brief Destructor.
         *
         * Initiates a graceful shutdown of the thread pool.
         */
        inline ~ThreadPool() noexcept {
            this->shutdown_internal(true);
        }

        // -------------------------------- Methods --------------------------------

        /**
         * @brief Enqueues a task with a specific priority.
         *
         * @tparam F Type of the callable.
         * @tparam Args Types of the arguments.
         * @param priority Integer priority. Higher values execute sooner.
         * @param f The callable function or task.
         * @param args Arguments to pass to the function.
         * @return std::future<return_type_t<F, Args...>> A future to the result of the task.
         */
        template<class F, class... Args>
        inline auto enqueue(int priority, F&& f, Args&&... args)
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

        /**
         * @brief Enqueues a task with default priority (0).
         *
         * @tparam F Type of the callable.
         * @tparam Args Types of the arguments.
         * @param f The callable function or task.
         * @param args Arguments to pass to the function.
         * @return std::future<return_type_t<F, Args...>> A future to the result of the task.
         */
        template<class F, class... Args>
        inline auto enqueue(F&& f, Args&&... args)
            -> std::future<return_type_t<F, Args...>>
        {
            return this->enqueue(DEFAULT_PRIORITY, std::forward<F>(f), std::forward<Args>(args)...);
        }

        /**
         * @brief Blocks the calling thread until the queue is empty and all running tasks are complete.
         *
         * @throws std::runtime_error If the pool is paused while tasks are still pending.
         * @throws std::logic_error If called from a worker thread (to prevent deadlock).
         */
        inline void wait() {
            this->checkDeadlock("wait");

            std::unique_lock<std::mutex> lock(queueMutex_);

            this->waitCV_.wait(lock, [this]() {
                bool isDone = this->taskQueue_.empty() && (this->runningTasks_.load(std::memory_order_acquire) == 0);
                bool isBlockedByPause = !this->taskQueue_.empty() && this->pauseFlag_.load(std::memory_order_acquire);
                return isDone || isBlockedByPause;
                });

            if (this->pauseFlag_.load(std::memory_order_acquire) && !this->taskQueue_.empty()) {
                throw std::runtime_error("ThreadPool is paused with pending tasks.");
            }

            assert(this->taskQueue_.empty() && "Wait finished but queue is not empty.");
            assert(this->runningTasks_.load(std::memory_order_acquire) == 0 && "Wait finished but tasks are running.");
        }

        /**
         * @brief Pauses the processing of new tasks.
         *
         * Tasks currently running are not interrupted.
         */
        inline void pause() {
            this->pauseFlag_.store(true, std::memory_order_release);
            this->waitCV_.notify_all();
        }

        /**
         * @brief Resumes the processing of tasks.
         */
        inline void resume() {
            this->pauseFlag_.store(false, std::memory_order_release);
            this->queueCV_.notify_all();
        }

        /**
         * @brief Clears all waiting tasks from the queue.
         *
         * @note Associated std::futures will receive a std::future_error (broken_promise).
         * @note Recommended to call pause() before clearing the queue.
         */
        inline void clearQueue() {
            std::unique_lock<std::mutex> lock(this->queueMutex_);
            std::priority_queue<Task, std::vector<Task>, TaskCompare> empty_queue;
            this->taskQueue_.swap(empty_queue);

            // If no tasks are running, notify wait calls that the pool is idle
            if (this->runningTasks_.load(std::memory_order_acquire) == 0) {
                this->waitCV_.notify_all();
            }
        }

        /**
         * @brief Shuts down the thread pool (Graceful).
         *
         * Waits for all pending tasks to complete before destroying threads.
         * A new ThreadPool instance is required to restart operations after shutdown.
         *
         * @throws std::logic_error If called from a worker thread.
         */
        inline void shutdown() {
            this->checkDeadlock("shutdown");
            this->shutdown_internal(false);
        }

        /**
         * @brief Terminates the thread pool (Immediate).
         *
         * Discards remaining tasks in the queue and destroys threads once current tasks finish.
         *
         * @throws std::logic_error If called from a worker thread.
         */
        inline void terminate() {
            this->checkDeadlock("terminate");
            this->shutdown_internal(true);
        }

        // ----------------------------- Status & Stats -----------------------------

        /**
         * @brief Gets the number of tasks currently waiting in the queue.
         * @return size_t Queue size.
         */
        inline size_t getQueueSize() const {
            std::unique_lock<std::mutex> lock(this->queueMutex_);
            return this->taskQueue_.size();
        }

        /**
         * @brief Gets the total number of worker threads.
         * @return size_t Thread count.
         */
        inline size_t getThreadCount() const { return this->threadCount_; }

        /**
         * @brief Gets the number of tasks currently being executed.
         * @return int Number of running tasks.
         */
        inline int getRunningTasks() const { return this->runningTasks_.load(std::memory_order_acquire); }

        /**
         * @brief Checks if the pool is paused.
         * @return true if paused, false otherwise.
         */
        inline bool isPaused() const { return this->pauseFlag_.load(std::memory_order_acquire); }

        /**
         * @brief Checks if the pool has been stopped/shutdown.
         * @return true if stopped, false otherwise.
         */
        inline bool isStopped() const {
            return this->stopFlag_.load(std::memory_order_acquire);
        }

        /// @brief Delete copy constructor (Forbidden).
        ThreadPool(const ThreadPool&) = delete;

        /// @brief Delete copy assignment operator (Forbidden).
        ThreadPool& operator=(const ThreadPool&) = delete;

    private:

        /// @brief Comparator for the priority queue (Max Heap based on priority int).
        struct TaskCompare {
            inline bool operator()(const Task& lhs, const Task& rhs) const {
                return lhs.first < rhs.first;
            }
        };

        /**
         * @brief Internal method to push a task onto the queue.
         *
         * @param priority Priority level.
         * @param work_wrapper Shared pointer to the work function.
         * @throws std::runtime_error If the pool is already stopped.
         */
        inline void queueTask(int priority, WorkPtr work_wrapper) {
            assert(work_wrapper && "Attempted to enqueue an empty task.");

            {   // Lock
                std::unique_lock<std::mutex> lock(this->queueMutex_);

                // shutdown status
                if (this->stopFlag_.load(std::memory_order_acquire)) {
                    throw std::runtime_error("Cannot enqueue task: ThreadPool is shut down.");
                }

                this->taskQueue_.emplace(priority, std::move(work_wrapper));
            }   // Unlock

            // Notify one waiting worker
            if (!this->pauseFlag_.load(std::memory_order_acquire)) {
                this->queueCV_.notify_one();
            }
        }

        /**
         * @brief Checks if the current thread is a worker thread to prevent deadlocks.
         *
         * @param callerName Name of the calling function for error reporting.
         * @throws std::logic_error If called from a worker thread.
         */
        inline void checkDeadlock(const char* callerName) {
            const std::thread::id this_id = std::this_thread::get_id();
            bool is_worker = false;

            for (const auto& t : this->threads_) {
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

        /**
         * @brief Internal shutdown logic.
         *
         * @param immediate If true, clears the task queue immediately. If false, waits for queue to empty.
         */
        inline void shutdown_internal(bool immediate) {
            {   // Lock
                std::unique_lock<std::mutex> lock(this->queueMutex_);

                if (this->stopFlag_.load(std::memory_order_acquire)) return;

                this->stopFlag_.store(true, std::memory_order_release);
                this->pauseFlag_.store(false, std::memory_order_release);

                if (immediate) {  // Clear task queue
                    std::priority_queue<Task, std::vector<Task>, TaskCompare> empty;
                    std::swap(this->taskQueue_, empty);
                }
            }   // Unlock

            this->queueCV_.notify_all();
            this->waitCV_.notify_all();

            auto this_id = std::this_thread::get_id();

            for (auto& t : this->threads_) {
                if (t.joinable()) {
                    if (t.get_id() == this_id) {  // Joining self thread cause deadlock
                        t.detach();
                    }
                    else {
                        t.join();
                    }
                }
            }
            this->threads_.clear();
        }

        /**
         * @brief The main loop executed by worker threads.
         *
         * Continously fetches and executes tasks from the queue.
         */
        inline void workerLoop() {
            while (true) {
                //Work work;
                WorkPtr work_ptr;

                {   // Lock
                    std::unique_lock<std::mutex> lock(this->queueMutex_);

                    // Wait condition
                    this->queueCV_.wait(lock, [this]() {
                        return this->stopFlag_.load(std::memory_order_acquire) || (!this->pauseFlag_.load(std::memory_order_acquire) && !this->taskQueue_.empty());
                        });

                    // Exit condition
                    if (this->stopFlag_.load(std::memory_order_acquire) && this->taskQueue_.empty()) { return; }

                    // Pause condition
                    if (this->pauseFlag_.load(std::memory_order_acquire)) { continue; }

                    if (!this->taskQueue_.empty()) {
                        //work = std::move(taskQueue_.top().second);  // std::move const warning, taskQueue_.top().second is const Work& type
                        //work = taskQueue_.top().second;  // copy is heavy
                        //work = std::move(const_cast<Work&>(taskQueue_.top().second));  // const_cast risk
                        work_ptr = this->taskQueue_.top().second;
                        this->taskQueue_.pop();

                        //assert(work && "Popped empty task from queue.");
                        assert(work_ptr && "Popped empty task from queue.");

                        this->runningTasks_.fetch_add(1, std::memory_order_release);
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
                    std::unique_lock<std::mutex> lock(this->queueMutex_);

                    int prevCount = this->runningTasks_.fetch_sub(1, std::memory_order_release);
                    assert(prevCount > 0 && "Running tasks count underflow");

                    // Decrement running task count
                    // If this was the last running task, check if we need to notify wait
                    if (prevCount == 1) {
                        if (this->taskQueue_.empty()) {
                            this->waitCV_.notify_all();  // Notify waiting threads (pool is idle)
                        }
                    }
                }   // Unlock
            }   // End of while
        }

    private:
        std::atomic<bool> stopFlag_{ false };       ///< Signals workers to terminate permanently
        std::atomic<bool> pauseFlag_{ false };      ///< Signals workers to temporarily stop consuming tasks
        std::atomic<int> runningTasks_{ 0 };        ///< Counter for currently executing tasks

        size_t threadCount_ = 0;                    ///< Target number of worker threads
        std::vector<std::thread> threads_;          ///< Container for worker threads

        // Priority queue of tasks, sort by priority
        std::priority_queue<Task, std::vector<Task>, TaskCompare> taskQueue_;

        mutable std::mutex queueMutex_;             ///< Mutex to protect the task queue
        std::condition_variable queueCV_;           ///< CV to signal workers when a new task arrives (or shutdown)
        std::condition_variable waitCV_;            ///< CV to signal threads waiting on wait() when the pool becomes idle
    };  // class end

}  // multi-threading

#endif  // CPP_MT_THREAD_POOL_HPP
