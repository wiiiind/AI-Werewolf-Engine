//
// Created by FengYijia on 26-2-6.
//

#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "http_conn.h"
#include "log.h"

// 前向声明
struct client_data;

class util_timer {
public:
    util_timer(): prev(NULL), next(NULL) {}

public:
    time_t expire;                  // 闹钟响的时间
    void (*cb_func)(client_data*);  // 要执行的函数指针
    client_data* user_data;         // 闹钟持有者

    util_timer* prev;
    util_timer* next;
};

struct client_data {
    sockaddr_in address;
    int sockfd;
    util_timer* timer;
};

class sort_time_lst {
private:
    util_timer* head;
    util_timer* tail;

public:
    sort_time_lst() : head(NULL), tail(NULL) {}
    ~sort_time_lst() {
        util_timer* tmp = head;
        while (tmp) {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    void add_timer(util_timer* timer) {
        if (!timer) return;
        if (!head) {    // 链表为空
            head = tail = timer;
            return;
        }
        if (timer->expire < head->expire) {
            timer->next = head;
            head->prev = timer;

            head = timer;
            return;
        }
        // 否则，插入合适的位置
        add_timer(timer, head);
    }

void adjust_timer(util_timer* timer) {
        if (!timer) return;
        util_timer* tmp = timer->next;
        // 尾部或者不需要调整的情况
        if (!tmp || (timer->expire < tmp->expire)) return;

        // 头节点
        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer);
        }
        else {  // 中间节点
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }

    void del_timer(util_timer* timer) {
        if (!timer) return;

        // 一个节点
        if ((timer == head) && (timer == tail)) {
            delete timer;
            head = tail = NULL;
            return;
        }

        if (timer == head) {
            head = head ->next;
            head->prev = NULL;
            delete timer;
            return;
        }

        if (timer == tail) {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }

        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    void tick() {
        if (!head) return;

        char time_str[32];
        epoll_utils::get_format_time(time_str, sizeof(time_str));
        LOG_INFO("Timer tick: checking expired connections...");

        time_t cur = time(NULL);    // 当前时间
        util_timer* tmp = head;
        while (tmp) {
            // 未到期
            if (cur<tmp->expire) {
                break;
            }
            // 到期
            tmp->cb_func(tmp->user_data);
            // 删除完，释放删除用的tmp
            head = tmp->next;
            if (head) {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    // 插入timer到lst_head后面
    void add_timer(util_timer* timer, util_timer* lst_head) {
        util_timer* prev = lst_head;
        util_timer* tmp = prev->next;
        while (tmp) {
            if (timer->expire < tmp->expire) {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        // 要插入的节点晚于所有节点
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
};

#endif //LST_TIMER_H
