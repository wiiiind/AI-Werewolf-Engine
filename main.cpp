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

const int MAX_FD = 65536;           // 最大文件描述符数量
const int MAX_EVENT_NUMBER = 10000; // 监听的最大事件数

// 外部声明在 http_conn.cpp 中定义的辅助函数
extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);

// 信号处理
void addsig(int sig, void (*handler)(int), bool restart = true) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handler;
    if (restart) {
        sa.sa_flags |= SA_RESTART;
    }
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
}

void show_error(int connfd, const char* info) {
    printf("%s\n", info);
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

int main(int argc, char* argv[]) {
    setbuf(stdout, NULL);
    int port = 9006;    // 默认端口
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // 1. 忽略SIGPIPE信号
    addsig(SIGPIPE, SIG_IGN);

    // 2. 创建线程池
    threadpool<http_conn> *pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch (...) {
        return 1;
    }

    // 3. 分配http_conn对象
    http_conn* users = new http_conn[MAX_FD];
    assert(users);

    int listenfd  = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);

    // 4. 端口复用
    struct linger tmp = {1,0};
    setsockopt(listenfd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);

    ret = listen(listenfd, 5);
    assert(ret >= 0);

    // 5. 创建epoll对象
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    // 注册listenfd
    addfd(epollfd, listenfd, false);

    // 把epoll_fd赋值给http_conn，让所有http_conn访问同一个epollfd
    http_conn::m_epollfd = epollfd;

    while (true) {
        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);  // 开始监听：让epollfd监听，监听结果放到events.data()，最大监听数量，超时设置：-1为“挂起”，0为只看一次，>0为具体等待时间
        // 返回值number是监听到有用结果的数量

        if (number < 0) {
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 1. 新客户端连接
            if (sockfd == listenfd) {
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

                // 初始化连接
                users[connfd].init(connfd, client_address);
            }

            // 2. 对方异常断开或错误
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                users[sockfd].close_conn();
            }

            // 3. 可读数据
            else if (events[i].events & EPOLLIN) {
                // 主线程中一次性读完数据
                if (users[sockfd].read()) {
                    // 读完后让线程池的线程处理
                    pool->append(users + sockfd);
                }
                else {  // 读失败
                    users[sockfd].close_conn();
                }
            }

            // 4. 可写数据
            else if (events[i].events & EPOLLOUT) {
                // 主线程中一次性发送完数据
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete[] users;
    delete[] pool;
    return 0;
}