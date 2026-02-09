#include <iostream>
#include <vector>
#include <sys/socket.h> // 核心socket头文件
#include <netinet/in.h> // sockaddr_in 结构体
#include <arpa/inet.h>  // inet_addr 等函数
#include <unistd.h>     // close, read, write
#include <cstring>      // memset
#include <sys/epoll.h>  // 核心 epoll 头文件
#include <fcntl.h>      // fcntl 设置非阻塞

#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"
#include "epoll_utils.h"

const int MAX_FD = 65536;           // 最大文件描述符数量
const int MAX_EVENT_NUMBER = 10000; // 监听的最大事件数
const int TIMESLOT = 5;             // 超时时间

static int pipefd[2];               // 管道
static sort_time_lst timer_lst;     // 定时器
static int epollfd;                 // Epoll文件描述符

// 往管道里写数据
void sig_handler(int sig) {
    int saved_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = saved_errno;
}
// 信号注册
void addsig(int sig, void (*handler)(int), bool restart = true) {
    struct sigaction sa;                        // 记录内核处理信号的方法的结构体
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;              // 设置“自动重启被干扰的系统调用”
    }
    sigfillset(&sa.sa_mask);                    // sa_mask代表处理该信号时允许处理的别的信号，是一个位图，全部设为1可以屏蔽其他信号，原子地处理该信号
    assert(sigaction(sig, &sa, NULL) != -1);    // 把sa绑定到sig上
}

// 定时器回调：处理过期链接
void cb_func(client_data* user_data) {
    char time_str[32];
    epoll_utils::get_format_time(time_str, sizeof(time_str));

    printf("[%s] Connection timeout: closing fd %d\n", time_str, user_data->sockfd);

    epoll_utils::removefd(epollfd, user_data->sockfd);
    assert(user_data);
    http_conn::m_user_count--;
}

// 定时任务：处理链表，重置闹钟
void timer_handler() {
    timer_lst.tick();
    alarm(TIMESLOT);
}

void show_error(int connfd, const char* info) {
    printf("%s\n", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char* argv[]) {
    setbuf(stdout, NULL);        // 禁用缓冲

    int port = 9006;                 // 默认端口
    if (argc > 1) {
        port = atoi(argv[1]);        // 传入的是字符串，转为int
    }

    // 1. 忽略SIGPIPE信号
    addsig(SIGPIPE, SIG_IGN);    // 向断开连接的客户端发信号，内核会发送SIGPIPIE信号，不屏蔽会导致服务器被系统强制结束

    // 2. 创建线程池
    threadpool<http_conn> *pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch (...) {
        return 1;
    }

    // 3. 分配内存
    http_conn* users = new http_conn[MAX_FD];
    assert(users);

    // 分配client_data，下标对应sockfd
    client_data* users_timer = new client_data[MAX_FD];

    // 4. 网络编程基础
    int listenfd  = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    // 端口复用
    struct linger tmp = {1,0};  // 跳过服务器关闭时四次回收的TIME_WAIT
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));


    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    // 设置listenfd要监听的端口信息
    int ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);
    // 开始监听
    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // 5. 创建epoll对象
    epoll_event events[MAX_EVENT_NUMBER];     // 存储epoll监听到的事件的数组

    epollfd = epoll_create(5);            // 创建一个epoll
    assert(epollfd != -1);
    epoll_utils::addfd(epollfd, listenfd, false, 0);   // 让epoll监听listenfd的事件

    // 把epoll_fd赋值给http_conn，让所有http_conn访问同一个epollfd
    http_conn::m_epollfd = epollfd;

    // 监听listenfd
    epoll_utils::addfd(epollfd, listenfd, true, 0); // 不需要ET和ONESHOT，传0

    // 6. 创建管道pipefd（pipefd[0]读，pipefd[1]写）
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);  // Unix域套接字，流式传输，全双工管道
    assert(ret != -1);

    // 写端非阻塞
    epoll_utils::setnonblocking(pipefd[1]);

    // 读端不需要ET
    epoll_utils::addfd(epollfd, pipefd[0], false, 0);

    // 7. 信号处理
    addsig(SIGALRM, sig_handler, false);
    addsig(SIGTERM, sig_handler, false);

    // 闹钟
    alarm(TIMESLOT);

    bool timeout = false;
    bool stop_server = false;

    while (!stop_server) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);  // 开始监听：让epollfd监听，监听结果放到events.data()，最大监听数量，超时设置：-1为“挂起”，0为只看一次，>0为具体等待时间
        // 返回值number是监听到有用结果的数量

        if (number < 0 && errno != EINTR) {
            // epoll_wait会卡住，直到见监听内容，因此执行到这里理应number大于0
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 1. 新客户端连接
            if (sockfd == listenfd) {
                // 为每个新连接创建connfd
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                if (connfd < 0) {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD) {
                    show_error(connfd, "Internal server busy");
                    continue;
                }

                char time_str[32];
                epoll_utils::get_format_time(time_str, sizeof(time_str));

                // 打印新连接信息，包括 IP 和端口
                char client_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &client_address.sin_addr, client_ip, INET_ADDRSTRLEN);
                printf("[%s] New connection established: fd %d, IP %s, Port %d\n",
                        time_str, connfd, client_ip, ntohs(client_address.sin_port));

                // 初始化连接
                users[connfd].init(connfd, client_address);

                // 初始化定时器
                util_timer* timer = new util_timer;
                timer->user_data = &users_timer[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;

                users_timer[connfd].address = client_address;
                users_timer[connfd].sockfd = connfd;
                users_timer[connfd].timer = timer;

                timer_lst.add_timer(timer);
            }

            // 2. 信号处理
            else if ((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)) {
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    continue;
                } else if (ret == 0) {
                    continue;
                } else {
                    for (int j = 0; j < ret; ++j) {
                        switch (signals[j]) {
                            case SIGALRM: {
                                timeout = true;
                                break;
                            }
                            case SIGTERM: {
                                stop_server = true;
                            }
                        }
                    }
                }
            }

            // 3. 可读数据
            else if (events[i].events & EPOLLIN) {
                util_timer* timer = users_timer[sockfd].timer;
                // 主线程中一次性读完数据
                if (users[sockfd].read()) {
                    // 若有数据，重置定时器
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                    // 读完后让线程池的线程处理
                    pool->append(users + sockfd);
                }
                else {  // 读失败
                    users[sockfd].close_conn();
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }
            }

            // 4. 可写数据
            else if (events[i].events & EPOLLOUT) {
                util_timer* timer = users_timer[sockfd].timer;
                // 主线程中一次性发送完数据
                if (!users[sockfd].write()) {
                    // 若有数据，重置定时器
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                    }
                } else {
                    cb_func(&users_timer[sockfd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
            // 处理定时事件
            if (timeout) {
                timer_handler();
                timeout = false;

        }
    }
    close(epollfd);
    close(listenfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;

}