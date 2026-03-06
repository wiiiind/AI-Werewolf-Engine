#include "support/TaskExecutor.h"

#include <stdexcept>

TaskExecutor::TaskExecutor(std::size_t worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("TaskExecutor requires at least one worker");
    }

    m_workers.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        m_workers.emplace_back([this]() { worker_loop(); });
    }
}

TaskExecutor::~TaskExecutor() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }

    m_condition.notify_all();
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

std::size_t TaskExecutor::worker_count() const {
    return m_workers.size();
}

void TaskExecutor::worker_loop() {
    while (true) {
        std::function<void()> task;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this]() { return m_stopping || !m_tasks.empty(); });

            if (m_stopping && m_tasks.empty()) {
                return;
            }

            task = std::move(m_tasks.front());
            m_tasks.pop();
        }

        task();
    }
}
