#ifndef TASK_EXECUTOR_H
#define TASK_EXECUTOR_H

#include <condition_variable>
#include <cstddef>
#include <future>
#include <functional>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class TaskExecutor {
public:
    explicit TaskExecutor(std::size_t worker_count);
    ~TaskExecutor();

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;

    template <typename Fn>
    auto submit(Fn&& fn) -> std::future<std::invoke_result_t<Fn>> {
        using ResultType = std::invoke_result_t<Fn>;

        auto task = std::make_shared<std::packaged_task<ResultType()>>(std::forward<Fn>(fn));
        std::future<ResultType> future = task->get_future();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stopping) {
                throw std::runtime_error("TaskExecutor is stopping");
            }
            m_tasks.push([task]() { (*task)(); });
        }

        m_condition.notify_one();
        return future;
    }

    std::size_t worker_count() const;

private:
    void worker_loop();

private:
    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    mutable std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_stopping = false;
};

#endif
