# C++ Priority ThreadPool

A simple, header-only ThreadPool implementation for C++ that manages tasks using a priority queue. This library is designed to make multi-threading environments easier to set up and use.

> **⚠️ Disclaimer**
>
> This project was written for educational purposes while studying C++.
> Please be aware that it **may contain errors, bugs, or non-optimal practices**. Use it with caution in production environments.

## Features

*   **Header-only**: No separate compilation required.
*   **Priority Scheduling**: Tasks can be enqueued with a priority level (Higher value = Higher priority).
*   **Modern C++**: Supports C++11 and later (automatically detects `std::invoke_result` vs `std::result_of`).
*   **Task Management**:
    *   Supports `std::future` for retrieving return values from tasks.
    *   Pause and Resume execution.
    *   Wait for all tasks to complete.
    *   Graceful shutdown or immediate termination.
*   **Safety**: Includes basic deadlock detection checks (e.g., preventing `wait()` calls from inside a worker thread).

## Requirements

*   **C++11** or later.
*   Standard Template Library (STL).

## Installation

Since this is a header-only library, simply copy the code into a file named `ThreadPool.hpp` (or any name you prefer) and include it in your project.

```cpp
#include "ThreadPool.hpp"
```

## Usage Example

```cpp
#include <iostream>
#include "ThreadPool.hpp"

void simple_task(int id) {
    std::cout << "Task " << id << " is running.\n";
}

int return_task(int a, int b) {
    return a + b;
}

int main() {
    // Initialize pool with 4 threads (or hardware default if 0)
    mt::ThreadPool pool(4);

    // 1. Enqueue tasks with default priority (0)
    pool.enqueue(simple_task, 1);

    // 2. Enqueue tasks with custom priority
    // Priority: 8 (High) -> Will run before priority 0 or -5
    pool.enqueue(8, simple_task, 2); 
    
    // Priority: -5 (Low)
    pool.enqueue(-5, simple_task, 3);

    // 3. Get return value using std::future
    std::future<int> result = pool.enqueue(return_task, 10, 20);
    std::cout << "Result: " << result.get() << std::endl; // Outputs 30

    // 4. Wait for all tasks to finish
    pool.wait();

    // 5. Pause and Resume
    pool.pause();
    // ... Add tasks safely without them starting immediately ...
    pool.resume();

    return 0;
}
```

## API Reference

### `ThreadPool(size_t threadCount = 0)`
Constructs the thread pool. If `threadCount` is 0, it defaults to `std::thread::hardware_concurrency()`.

### `enqueue(int priority, F&& f, Args&&... args)`
Adds a task to the queue with a specific priority. Returns a `std::future`.
*   **priority**: Integer value. Higher numbers are executed first.
*   **Default priority**: `0` (if using the overload without priority argument).

### `wait()`
Blocks the calling thread until the queue is empty and all currently running tasks are finished.

### `pause() / resume()`
Pauses or resumes the consumption of tasks from the queue. Currently running tasks are not interrupted.

### `clearQueue()`
Removes all pending tasks from the queue. Associated futures will receive a broken promise error.

### `shutdown()` / `terminate()`
*   **shutdown()**: Graceful exit. Waits for workers to finish.
*   **terminate()**: Clears remaining tasks and stops as soon as possible.

## References

This implementation was inspired by and referenced the following resource:
*   [Modoocode - C++ ThreadPool Implementation](https://modoocode.com/285)

