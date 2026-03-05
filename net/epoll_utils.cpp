//
// Created by FengYijia on 26-2-6.
//

#include "epoll_utils.h"
#include <time.h>

namespace epoll_utils {
    // 设置文件描述符为非阻塞
    int setnonblocking(int fd) {
        int old_option = fcntl(fd, F_GETFL);    // 获取旧的标志位
        int new_option = old_option | O_NONBLOCK;    // 把“非阻塞”通过“或”操作设为开启状态
        fcntl(fd, F_SETFL, new_option);         // 对fd执行“F_SETFL”（设置）操作，参数为new_option
        // 现在已经修改好了
        return old_option;                           // 返回旧的标志位（可能调用函数有需要）
    }

    // 把文件描述符fd添加到epoll的监控列表
    // enable_et: 是否启用ET（边缘触发，即“只有新的可读事件才触发”）
    void addfd(int epollfd, int fd, bool one_shot, int trig_mode) {
        epoll_event event{};                        // 创建一个epoll事件的结构体
        event.data.fd = fd;                         // 把他绑定到要监听的fd

        // 默认开启读事件（ET）和挂断检测
        if (trig_mode == 1) {
            event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
        }
        else {
            event.events = EPOLLIN | EPOLLRDHUP;
        }

        if (one_shot)
            event.events |= EPOLLONESHOT;

        setnonblocking(fd);                         // 把fd设为非阻塞
        // 非阻塞：recv(fd)时，如果没有数据，线程会挂起而不是卡死
        epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);  // 注册事件
        // 这里只需要传event指针，避免再次复制，event在addfd结束会被自动销毁
    }

    // 从epoll中移除文件描述符
    void removefd(int epollfd, int fd) {
        epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
        close(fd);
    }

    void modfd(int epollfd, int fd, int ev, int trig_mode) {
        epoll_event event;
        event.data.fd = fd;

        // 需要保留原来的 ET 模式设置
        if (trig_mode == 1)
            event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
        else
            event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

        epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
    }

    void get_format_time(char *buf, int len) {
        time_t now = time(0);
        struct tm *ltm = localtime(&now);
        strftime(buf, len, "%Y-%m-%d %H:%M:%S", ltm);
    }
}
