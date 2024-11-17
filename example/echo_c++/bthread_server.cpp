

#include <iostream>
#include <unistd.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <fcntl.h>

#include "bthread/bthread.h"
#include "butil/logging.h"

// 设置套接字为非阻塞模式
void set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 处理客户端连接的线程函数
void* handle_client(void* args) {
    int client_fd = *(int*)args;
    delete (int*)args; // 释放传递的文件描述符指针

    char buffer[1024];
    while (true) {
        ssize_t n = read(client_fd, buffer, sizeof(buffer));
        if (n > 0) {
            // 回显收到的消息
            write(client_fd, buffer, n);
        } else if (n == 0) {
            // 客户端关闭连接
            close(client_fd);
            LOG(INFO) << "Client disconnected, fd: " << client_fd;
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 数据读取完毕，继续等待
                bthread_usleep(1000); // 休眠 1 毫秒，避免忙等待
                continue;
            } else {
                // 其他错误
                close(client_fd);
                LOG(ERROR) << "Read error on fd " << client_fd << ", error: " << strerror(errno);
                break;
            }
        }
    }
    return nullptr;
}

// 主循环线程函数
void* server_loop(void* args) {
    int port = *(int*)args;

    // 创建监听套接字
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOG(ERROR) << "Failed to create socket: " << strerror(errno);
        return nullptr;
    }

    // 设置地址可重用
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定地址
    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(listen_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG(ERROR) << "Failed to bind socket: " << strerror(errno);
        close(listen_fd);
        return nullptr;
    }

    // 开始监听
    if (listen(listen_fd, 128) < 0) {
        LOG(ERROR) << "Failed to listen: " << strerror(errno);
        close(listen_fd);
        return nullptr;
    }

    LOG(INFO) << "Server is listening on port " << port;

    // 创建 epoll 实例
    int epfd = epoll_create1(0);
    if (epfd < 0) {
        LOG(ERROR) << "Failed to create epoll: " << strerror(errno);
        close(listen_fd);
        return nullptr;
    }

    // 将监听套接字加入 epoll
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev) < 0) {
        LOG(ERROR) << "Failed to add listen_fd to epoll: " << strerror(errno);
        close(listen_fd);
        close(epfd);
        return nullptr;
    }

    // 事件循环
    const int MAX_EVENTS = 10;
    epoll_event events[MAX_EVENTS];
    while (true) {
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue; // 被信号中断，重新等待
            }
            LOG(ERROR) << "epoll_wait error: " << strerror(errno);
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd == listen_fd) {
                // 处理新的客户端连接
                sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);
                int client_fd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);
                if (client_fd >= 0) {
                    set_non_blocking(client_fd);
                    LOG(INFO) << "New client connected, fd: " << client_fd;

                    // 创建 bthread 处理客户端
                    bthread_t th;
                    int* pclient_fd = new int(client_fd);
                    if (bthread_start_background(&th, nullptr, handle_client, pclient_fd) != 0) {
                        LOG(ERROR) << "Failed to create bthread for client";
                        close(client_fd);
                        delete pclient_fd;
                    }
                } else {
                    LOG(ERROR) << "Failed to accept: " << strerror(errno);
                }
            }
        }
    }

    close(listen_fd);
    close(epfd);
    return nullptr;
}

int main(int argc, char* argv[]) {
    int port = 12345; // 默认端口
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // 初始化日志系统
    butil::InitGoogleLogging(argv[0]);

    // 创建服务器主循环线程
    bthread_t server_th;
    if (bthread_start_background(&server_th, nullptr, server_loop, &port) != 0) {
        LOG(ERROR) << "Failed to start server thread";
        return -1;
    }

    // 等待服务器线程结束（实际上不会结束）
    bthread_join(server_th, nullptr);

    return 0;
}
