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

const int MAX_EVENT_NUMBER = 10000;  // 监听的最大事件数
const int BUFFER_SIZE = 1024;

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
void addfd(int epollfd, int fd, bool enable_et) {
    epoll_event event{};                          // 创建一个epoll事件的结构体
    event.data.fd = fd;                         // 把他绑定到要监听的fd
    event.events = EPOLLIN;                     // 监听的事件是“可读事件”
    if (enable_et) {
        event.events |= EPOLLET;                // 把他设为ET
    }
    setnonblocking(fd);                         // 把fd设为非阻塞
                                                // 非阻塞：recv(fd)时，如果没有数据，线程会挂起而不是卡死
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);  // 注册事件
                                                // 这里只需要传event指针，避免再次复制，event在addfd结束会被自动销毁
}

class Task {
    public:
    void process() {
        std::cout << "Task is processing by thread: " << pthread_self() << std::endl;
    }
};

int main() {
    int port = 9006;    // 监听的端口

    // 1. 创建监听socket
    int listenfd = socket(PF_INET, SOCK_STREAM, 0); //IPV4，流式传输，自动选择协议（根据前两个参数）（最后自动选择TCP）
    if (listenfd < 0) {
        std::cerr << "socket error: " << strerror(errno) << std::endl;  // cerr类似cout，但专用于输出报错；strerror可以把数字的错误码翻译成文字
        return -1;
    }

    // 2. Bind：绑定地址
    sockaddr_in address{};
    address.sin_family = AF_INET;                   // IPV4
    address.sin_addr.s_addr = htonl(INADDR_ANY);    // INADDR_ANY：所有网卡；htonl：Host To Network Long，小端序转为大端序
    address.sin_port = htons(port);                 // htons：端口号转为大端序

    // 强制重用：允许服务器重启后，跳过上次断开连接的TIME_WAIT状态，无需等待
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(int));    // setsockopt是C类型函数，要求第三个参数传入一个指针

    if (bind(listenfd, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)) == -1) { // 把address绑定到listenfd，注意把sockaddr_in*（IPV4）强制转化为sockaddr*
        std::cerr << "bind error: " << strerror(errno) << std::endl;
    }

    // 3. 监听
    if (listen(listenfd, 5) == -1) {    // 让listenfd负责监听，5是Backlog，允许等候数量，这里5只是个参考，内核会自动调整
        std::cerr << "listen error: " << strerror(errno) << std::endl;
    }

    // 4. 创建epoll
    int epollfd = epoll_create(5);      // 这个5在这里其实也是powerless husband
    if (epollfd == -1) {
        std::cerr << "epoll_create error: " << strerror(errno) << std::endl;
    }

    threadpool<Task>* pool = nullptr;
    try {
        pool = new threadpool<Task>;
    } catch (...) {
        return 1;
    }

    std::vector<epoll_event> events(MAX_EVENT_NUMBER);  // 这里存储的不是监听的所有fd，而是监听到需要处理的fd

    addfd(epollfd, listenfd, false);  // 告诉epollfd要监听listenfd；先默认为LT，逻辑简单

    while (true) {
        int number = epoll_wait(epollfd, events.data(), MAX_EVENT_NUMBER, -1);  // 开始监听：让epollfd监听，监听结果放到events.data()，最大监听数量，超时设置：-1为“挂起”，0为只看一次，>0为具体等待时间
            // 返回值number是监听到有用结果的数量

        if (number < 0) {
            std::cerr << "epoll_wait error: " << strerror(errno) << std::endl;
        }

        for (int i = 0; i < number; i++) {
            int sockfd = events[i].data.fd;

            // 新连接：建立“已连接套接字”
            if (sockfd == listenfd) {
                sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);

                int connfd = accept(listenfd, reinterpret_cast<sockaddr*>(&client_address), &client_addrlength);    // 为新连接创建一个“已连接套接字”
                if (connfd == -1) {
                    std::cerr << "accept error: " << strerror(errno) << std::endl;
                }

                std::cout << "New client connected. fd: " << connfd << std::endl;
                addfd(epollfd, connfd, true);   // 把新建立的connfd放进epoll，复用listenfd
            }
            // 老连接：接收数据
            else if (events[i].events & EPOLLIN) {  // 检查：只有EPOLLIN为1才能进入分支
                char buf[BUFFER_SIZE];

                while (true) {  // ET：只通知一次，所以必须一次性读取完全部数据
                    std::memset(buf, 0, sizeof(buf));
                    int ret = recv(sockfd, buf, BUFFER_SIZE, 0);    //读取接受缓冲区（内核里）中，sockfd对应的信息，放入buf，长度为BUFFER_SIZE；recv一读完，内核里对应数据就会消失

                    if (ret < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // 数据读完了
                        }
                        std::cout << "Client error, close fd: " << sockfd << std::endl;
                        close(sockfd);
                        epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
                        break;
                    }
                    else if (ret == 0) {
                        std::cout << "Client closed connection. fd: " << sockfd << std::endl;
                        close(sockfd);
                        epoll_ctl(epollfd, EPOLL_CTL_DEL, sockfd, NULL);
                        break;
                    }
                    else {
                        /*std::cout << "Get: " << ret << " bytes: " << buf << std::endl;*/
                        std::cout << "Receive data, adding to thread pool..." << std::endl;
                        Task* task = new Task();
                        pool->append(task);
                        send(sockfd, buf, ret, 0);
                    }
                }
            }
        }
    }
}