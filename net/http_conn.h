//
// Created by FengYijia on 26-1-28.
//

#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

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
#include <sys/uio.h>

#include "locker.h"
#include "epoll_utils.h"


class http_conn {
public:
    static const int FILENAME_LEN = 200;        // 文件名长度
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓冲区

    enum METHOD {GET = 0, POST, PUT, DELETE};   // HTTP请求方法

    enum CHECK_STATE {                          // 解析时主状态机状态
        CHECK_STATE_REQUESTLINE = 0,            // 分析请求行
        CHECK_STATE_HEADER,                     // 分析请求头
        CHECK_STATE_CONTENT                     // 分析内容
    };

    enum HTTP_CODE {                            // HTTP请求结果
        NO_REQUEST,                             // 请求不完整
        GET_REQUEST,                            // 获得了完整的客户请求
        BAD_REQUEST,                            // 客户请求有语法错误
        NO_RESOURCE,                            // 没有资源
        FORBIDDEN_REQUEST,                      // 没有访问权限
        FILE_REQUEST,                           // 文件请求
        INTERNAL_ERROR,                         // 服务器内部错误
        CLOSED_CONNECTION                       // 客户端关闭连接
    };

    // 从状态机的三种可能状态，即行的读取状态
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    http_conn();
    ~http_conn();

public:
    void init(int sockfd, const sockaddr_in& addr);    // 初始化接收新连接
    void close_conn(bool real_close = true);     // 关闭连接
    void process();                              // 处理客户端操作
    bool read();                                 // 非阻塞读
    bool write();                                // 非阻塞写

private:
    void init();                                 // 初始化（内部）
    HTTP_CODE process_read();                    // 解析HTTP
    HTTP_CODE do_request();

    bool process_write(HTTP_CODE ret);           // 写HTTP应答
    LINE_STATUS parse_line();

    HTTP_CODE parse_request_line(char *text);

    HTTP_CODE parse_headers(char *text);

    HTTP_CODE parse_content(char *text);

    // 均被process_write调用
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();
    char* get_line() { return m_read_buf + m_start_line; }  // 获取当前行的首地址

public:
    // 将所有socket事件都注册到一个epoll内核事件表
    static int m_epollfd;
    static int m_user_count;                     // 统计用户数量

private:
    int m_sockfd;                                // HTTP连接的socket
    sockaddr_in m_address;                       // 对方socket地址

    char m_read_buf[READ_BUFFER_SIZE];           // 读缓冲区
    int m_read_idx;                              // 正在读字符的索引
    int m_checked_idx;                           // 分析的索引
    int m_start_line;                            // 解析的行的行头
    char m_write_buf[WRITE_BUFFER_SIZE];         // 写缓冲区
    int m_write_idx;                             // 待发送的字节数
    int bytes_have_send;                         // 已经发送的字节数
    int bytes_to_send;                           // 未发送的字节数

    CHECK_STATE m_check_state;                   // 主状态机所处状态
    METHOD m_method;                             // 请求方法

    char m_real_file[FILENAME_LEN];              // 请求文件路径
    char* m_url;                                 // 请求文件名
    char* m_version;                             // HTTP协议版本
    char* m_host;                                // 主机名
    int m_content_length;                        // 请求消息体长度
    bool m_linger;                               // 是否要求保持连接

    char* m_file_address;                        // 目标文件被mmap到内存中的位置
    struct stat m_file_stat;                     // 目标文件状态
    struct iovec m_iv[2];                        // 用writev来写
    int m_iv_count;
};

#endif