//
// Created by FengYijia on 26-1-29.
//

#include "http_conn.h"

#include "log.h"


const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录，指向你刚才建的 resources 文件夹
const char* doc_root = "/home/fengyijia/projects/MyTinyWebServer/resources";

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close) {
    if (real_close && (m_sockfd != -1)) {
        unmap();
        epoll_utils::removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}


http_conn::http_conn() {
}

http_conn::~http_conn() {
}

// 写操作：将响应头和文件内容一并发给浏览器
bool http_conn::write() {
    int temp = 0;

    if (bytes_to_send == 0) {
        bytes_to_send = m_iv[0].iov_len + m_iv[1].iov_len;
    }

    // 如果没有东西要写，这逻辑肯定有问题
    if (bytes_to_send == 0) {
        epoll_utils::modfd(m_epollfd, m_sockfd, EPOLLIN, 1);
        init();
        return true;
    }

    while (1) {
        // writev: 聚集写。把 m_iv 里的多块内存数据，一次性写入 socket
        temp = writev(m_sockfd, m_iv, m_iv_count);

        if (temp <= -1) {
            // 如果缓冲区满了（EAGAIN），那就以后再发
            if (errno == EAGAIN) {
                // 重新注册写事件，等一会再试
                epoll_utils::modfd(m_epollfd, m_sockfd, EPOLLOUT, 1);
                return true;
            }
            // 真的出错了（比如对方断开连接），那就释放 mmap 内存
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        // 【关键】处理已经发送完的数据
        // 这一部分逻辑比较繁琐，是因为 writev 可能一次没发完。
        // 如果第一块（响应头）发完了，还没发完第二块（文件）：
        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0; // 头已经不需要再发了
            // 更新第二块内存的起始位置和剩余长度
            // 这里的逻辑稍微有点绕：实际上是调整指针偏移
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            // 如果连响应头都没发完
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        // 如果所有数据都发完了
        if (bytes_to_send <= 0) {
            unmap(); // 发完了，归还内存
            epoll_utils::modfd(m_epollfd, m_sockfd, EPOLLIN, 1); // 重新监听读事件，准备接收下一次请求

            if (m_linger) {
                init(); // 如果是 keep-alive，初始化状态机，但不关闭连接
                return true;
            } else {
                return false; // 如果不是，返回 false 让上层关闭连接
            }
        }
    }
}

void http_conn::init(int sockfd, const sockaddr_in& addr) {
    m_sockfd = sockfd;
    m_address = addr;

    // 端口复用
    int reuse = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    epoll_utils::addfd(m_epollfd, sockfd, true, 1);
    m_user_count++;

    init();
}

void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    bytes_have_send = 0;
    bytes_to_send = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

// 循环读取
bool http_conn::read() {
    if (m_read_idx == READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据了
                break;
            }
            return false;
        }
        else if (bytes_read == 0) {
            // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
        LOG_DEBUG("读取到了数据: %s", m_read_buf);
        return true;
    }
    return true;
}

void http_conn::process() {
    LOG_DEBUG("线程池正在处理请求...");
    // 1. 解析HTTP请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        // 请求直到完整接收
        epoll_utils::modfd(m_epollfd, m_sockfd, EPOLLIN, 1);
        return;
    }

    // 2. 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }

    // 3. 注册监听
    epoll_utils::modfd(m_epollfd, m_sockfd, EPOLLOUT, 1);
}

bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_title));
            if (!add_content(error_500_form)) return false;
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) return false;
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) return false;
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) return false;
            break;

        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            if (m_file_stat.st_size!=0) {
                add_headers(m_file_stat.st_size);

                // m_iv[0]指向响应头
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;

                // m_iv[1]指向文件内容
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;

                m_iv_count = 2;
                return true;
            }
            else {
                const char* ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string)) return false;
            }
        }
    // 如果不是文件请求（比如报错了），我们只发响应头那一块内存
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 从状态机：把字符流切成一行行
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; m_checked_idx++) {
        temp = m_read_buf[m_checked_idx];

        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0'; // 把\r改为\0
                m_read_buf[m_checked_idx++] = '\0'; // 把\n改为\0
                return LINE_OK;
            }
            return LINE_BAD;    // 语法错误
        }
        else if (temp == '\n') {
            if ((m_checked_idx > 0) && (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx++] = '\0'; // 把\r改为\0
                m_read_buf[m_checked_idx++] = '\0'; // 把\n改为\0
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

// 解析HTTP请求行
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    // strpbrk: 在 text 中搜索 " \t" (空格或tab) 第一次出现的位置
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return BAD_REQUEST;
    }

    // 把空格变成 \0，这样 text 就截断变成了 "GET"
    *m_url++ = '\0';

    // 取出方法
    char* method = text;
    if (strcasecmp(method, "GET") == 0) { // strcasecmp 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST; // 我们只支持 GET
    }

    // 跳过 method 后面多余的空格
    m_url += strspn(m_url, " \t");

    // 查找 HTTP 版本号
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");

    // 仅支持 HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;
    }

    // 处理 URL 可能带有的 http:// 前缀
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/'); // 找到域名的结束位置
    }

    // 如果请求的是根目录，则默认指向 index.html
    if (strcmp(m_url, "/") == 0) {
        m_url = (char*)"/index.html";
    }

    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    // 解析完请求行后，状态转移到 -> 检查头部
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST; // 继续解析
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) {
    // 遇到空行，说明头部解析完毕
    if (text[0] == '\0') {
        // 如果有内容长度（POST请求），需要继续解析内容
        // 但我们只实现GET，通常 content_length 为 0
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        // 否则说明请求彻底解析完了
        return GET_REQUEST;
    }
    // 处理 Connection 头部
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true; // 保持连接
        }
    }
    // 处理 Content-Length 头部
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    // 处理 Host 头部
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else {
        // LOG_DEBUG("Oop! unknow header: %s", text);
    }
    return NO_REQUEST;
}

// 解析请求体（GET请求通常没这个，只有 POST 有）
http_conn::HTTP_CODE http_conn::parse_content(char* text) {
    // 判断是否读取了完整的消息体
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

// 主状态机：解析 HTTP 请求
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;

    // 循环条件：
    // 1. 解析到了完整的一行 (line_status == LINE_OK)
    // 2. 或者正在解析消息体 (m_check_state == CHECK_STATE_CONTENT)
    while (((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
        || ((line_status = parse_line()) == LINE_OK)) {

        // 获取一行数据
        text = get_line();

        // m_start_line 更新为下一行的起始位置
        m_start_line = m_checked_idx;

        // 打印一下日志，看看解析到了什么
        LOG_DEBUG("got 1 http line: %s", text);

        // 主状态机转移
        switch (m_check_state) {
            case CHECK_STATE_REQUESTLINE: {
                // 当前状态：正在分析请求行
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER: {
                // 当前状态：正在分析头部字段
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    // 解析完成！调用 do_request 寻找文件
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT: {
                // 当前状态：正在分析内容
                ret = parse_content(text);
                if (ret == GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: {
                return INTERNAL_ERROR;
            }
        }
        }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request() {
    // 1. 拼接绝对路径
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    LOG_INFO("正在读取文件: %s", m_real_file);

    // 2. 获取文件状态
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }

    // 3. 判断权限
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 4. 判断是不是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 5. 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);

    // 6. 使用mmap把文件映射到内存
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    close(fd);

    return FILE_REQUEST;
}

void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

bool http_conn::add_response(const char *format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }

    va_list arg_list;
    va_start(arg_list, format);

    // 把格式化的数据写回 m_write_buf + m_write_idx 处
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    if (len >= WRITE_BUFFER_SIZE - 1 - m_write_idx) {
        return false;   // 缓冲区满了
    }

    m_write_idx += len;
    va_end(arg_list);
    return true;
}

// 添加状态行
bool http_conn::add_status_line(int status, const char* title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

// 添加报头
bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char* content) {
    return add_response("%s", content);
}
bool http_conn::add_content_type() {
    return add_response("Content-Type: %s\r\n", "text/html");
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_linger();
    add_blank_line();
    return true;
}