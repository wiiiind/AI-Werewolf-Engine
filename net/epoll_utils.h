//
// Created by FengYijia on 26-2-6.
//

#ifndef EPOLL_UTILS_H
#define EPOLL_UTILS_H

#include <sys/epoll.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

namespace epoll_utils {
    // 设置文件描述符非阻塞
    int setnonblocking(int fd);

    // 将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int trig_mode);

    // 从内核事件表删除描述符
    void removefd(int epollfd, int fd);

    // 将事件重置为EPOLLONESHOT
    void modfd(int epollfd, int fd, int ev, int trig_mode);

    // 获取当前格式化时间的字符串 [YYYY-MM-DD HH:MM:SS]
    void get_format_time(char* buf, int len);
}


#endif //EPOLL_UTILS_H
