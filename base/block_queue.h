//
// Created by FengYijia on 26-2-11.
//

#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <iostream>
#include <stdlib.h>
#include <pthread.h>
#include <sys/time.h>
#include <deque>    // 使用双端队列作为底层容器
#include "locker.h" // 使用你之前写的锁

template <class T>
class block_queue {
public:
    block_queue(int max_size = 1000) {
        if (max_size < 1) {
            exit(-1);
        }
        m_max_size = max_size;
    }
    ~block_queue() {
        m_mutex.lock();
        m_queue.clear();
        m_mutex.unlock();
    }

    void clear() {
        m_mutex.lock();
        m_queue.clear();
        m_mutex.unlock();
    }

    bool full() {
        m_mutex.lock();
        if (m_queue.size() >= m_max_size) {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }
    bool empty() {
        m_mutex.lock();
        if (m_queue.empty()) {
            m_mutex.unlock();
            return true;
        }
        m_mutex.unlock();
        return false;
    }

    // 返回队首、队尾
    bool front(T &value) {
        m_mutex.lock();
        if (m_queue.size() == 0) {
            m_mutex.unlock();
            return false;
        }
        value = m_queue.front();
        m_mutex.unlock();
        return true;
    }
    bool back(T &value) {
        m_mutex.lock();
        if (m_queue.size() == 0) {
            m_mutex.unlock();
            return false;
        }
        value = m_queue.back();
        m_mutex.unlock();
        return true;
    }

    int size() {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_queue.size();
        m_mutex.unlock();
        return tmp;
    }

    int max_size() {
        int tmp = 0;
        m_mutex.lock();
        tmp = m_queue.max_size();
        m_mutex.unlock();
        return tmp;
    }

    // 生产者
    bool push(const T &item) {
        m_mutex.lock();
        if (m_queue.size() >= m_max_size) {
            // 队列已满，通知消费者取出，这次push失败
            m_cond.broadcast();
            m_mutex.unlock();
            return false;
        }
        m_queue.push_back(item);
        m_cond.broadcast();
        m_mutex.unlock();
        return true;
    }

    // 消费者
    bool pop(T &item) {
        m_mutex.lock();
        while (m_queue.size() <= 0) {
            if (!m_cond.wait(m_mutex.get())) {
                m_mutex.unlock();
                return false;
            }
        }

        item = m_queue.front();
        m_queue.pop_front();
        m_mutex.unlock();
        return true;
    }

private:
    locker m_mutex;         // 互斥锁
    cond m_cond;            // 条件变量

    std::deque<T> m_queue;  // 双端队列
    int m_max_size;         // 最大容量
};

#endif //BLOCK_QUEUE_H
