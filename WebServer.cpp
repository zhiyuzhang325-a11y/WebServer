#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>
using namespace std;
#define BUFSIZE 10
#define MAXSIZE 1024

enum LOGLEVEL {
    DEBUG, // 调试
    INFO,  // 信息
    WARN,  // 警告
    ERROR, // 错误
    FATAL  // 致命
};

string getTimestamp() {
    auto now = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    return buf;
}

void logger(LOGLEVEL level, string message) {
    if (level == ERROR || level == FATAL) {
        cout << getTimestamp() << " " << level << ": " << message << " : " << strerror(errno) << endl;
    } else {
        cout << getTimestamp() << " " << level << ": " << message << endl;
    }
}

int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd) {
    epoll_event event;
    event.events |= EPOLLET;
    event.events |= EPOLLIN;
    event.data.fd = fd;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void ET(int epollfd, int listenfd, epoll_event *events, int numbers) {
    string buffer(BUFSIZE, '\0');
    for (int i = 0; i < numbers; i++) {
        int fd = events[i].data.fd;
        if (fd == listenfd) {
            sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int connfd = accept(listenfd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
            string client_ip(INET_ADDRSTRLEN, '\0');
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip.data(), client_ip.size());
            int client_port = ntohs(client_addr.sin_port);
            logger(INFO, client_ip + " " + to_string(client_port));
            addfd(epollfd, connfd);
        } else if (events[i].events & EPOLLIN) {
            memset(buffer.data(), '\0', BUFSIZE);
            int recvd = 0;
            while (1) {
                int n = recv(fd, buffer.data() + recvd, BUFSIZE - recvd, 0);
                if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        logger(INFO, "had read over, buffer is : " + buffer);
                        string response = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nHello\n";
                        send(fd, response.c_str(), response.size(), 0);
                        break;
                    } else {
                        logger(ERROR, "read failed");
                        close(fd);
                        break;
                    }
                } else if (n == 0) {
                    logger(WARN, "client is closed");
                    close(fd);
                    break;
                }
                recvd += n;
            }
        } else {
            logger(WARN, "something else happened");
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc <= 2) {
        logger(ERROR, "argv is too little");
        return -1;
    }

    string ip(argv[1]);
    int port = stoi(argv[2]);
    sockaddr_in addr;
    memset(&addr, '\0', sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        logger(ERROR, "socket failed");
    }
    int ret = bind(listenfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (ret < 0) {
        logger(ERROR, "bind faile");
    }
    ret = listen(listenfd, 1024);
    if (ret < 0) {
        logger(ERROR, "bind failed");
    }

    logger(INFO, "socket successed, and now is listening");

    epoll_event events[MAXSIZE];
    int epollfd = epoll_create(1);
    addfd(epollfd, listenfd);

    while (1) {
        int numbers = epoll_wait(epollfd, events, MAXSIZE, -1);
        if (numbers < 0) {
            logger(ERROR, "epoll_wait failed");
            break;
        }
        ET(epollfd, listenfd, events, numbers);
    }
    close(listenfd);
    close(epollfd);
    return 0;
}