//
// Created by FengYijia on 26-1-26.
//

#ifndef LOCKER_H
#define LOCKER_H

#include <exception>
#include <pthread.h>
#include <semaphore.h>

// 封装信号量
// P：-1，为0则阻塞；V：+1
class sem {
public:
    sem() {
        //  RAII (资源获取即初始化)
        if (sem_init(&m_sem, 0, 0) != 0) {  // 信号量地址，线程间共享，初始资源数
            throw std::exception();
        }
    }

    sem(int num) {
        if (sem_init(&m_sem, 0, num) != 0) {
            throw std::exception();
        }
    }

    ~sem() {
        sem_destroy(&m_sem);
    }

    // P操作：等待
    bool wait() {
        return sem_wait(&m_sem) == 0;   // m_sem值>0则会把值-1返回0（成功），否则线程阻塞直到值>0
    }

    // V操作：增加
    bool post() {
        return sem_post(&m_sem) == 0;
    }

private:
    sem_t m_sem;
};

// 互斥锁
class locker {
public:
    locker() {
        if (pthread_mutex_init(&m_mutex, NULL) != 0){
            throw std::exception();
        }
    }
    ~locker() {
        pthread_mutex_destroy(&m_mutex);
    }
    bool lock() {
        return pthread_mutex_lock(&m_mutex) == 0;
    }
    bool unlock() {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }
    pthread_mutex_t *get() {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
};

// 条件变量
class cond {
public:
    cond() {
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }
    ~cond() {
        pthread_cond_destroy(&m_cond);
    }

    // 等待条件变量
    bool wait(pthread_mutex_t *m_mutex) {
        int ret = 0;
        // 这一步包含了：释放mutex -> 睡觉 -> 被唤醒 -> 重新拿回mutex
        ret = pthread_cond_wait(&m_cond, m_mutex);
        return ret == 0;
    }

    // 唤醒一个
    bool signal() {
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {
        return pthread_cond_broadcast(&m_cond) == 0;
    }

private:
    pthread_cond_t m_cond;
};
#endif //LOCKER_H
