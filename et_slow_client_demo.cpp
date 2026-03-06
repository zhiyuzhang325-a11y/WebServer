#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>
using namespace std;
#define MAXSIZE 1024
int BUFSIZE = 4096;
string body(10 * 1024 * 1024, 'A');
string output = "HTTP/1.1 200 OK\r\nContent-Length: " + to_string(body.size()) + "\r\n\r\n" + body;

int epollfd = epoll_create(1);
struct Connection {
    int fd;
    string outbuf;
    bool readClosed = false;
    bool sendComplete = false;
};
unordered_map<int, Connection> conns;

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

void setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
}

void addfd(int fd) {
    epoll_event event;
    event.events = EPOLLET | EPOLLIN;
    event.data.fd = fd;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void modfd(int fd, auto events) {
    epoll_event event;
    event.events = events;
    event.data.fd = fd;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void trySend(Connection &conn) {
    if (conn.outbuf.empty()) {
        return;
    }
    logger(DEBUG, "trySend called for fd = " + to_string(conn.fd) + ", outbuf size = " + to_string(conn.outbuf.size()));
    while (!conn.outbuf.empty()) {
        int n = send(conn.fd, conn.outbuf.c_str(), conn.outbuf.size(), 0);
        if (n > 0) {
            conn.outbuf.erase(0, n);
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            logger(DEBUG, "write would block, enable EPOLLOUT and send later");
            modfd(conn.fd, EPOLLET | EPOLLIN | EPOLLOUT);
            return;
        } else {
            logger(ERROR, "send failed");
            close(conn.fd);
            conns.erase(conn.fd);
            return;
        }
    }
    conn.sendComplete = true;
    if (conn.readClosed) {
        logger(INFO, "send complete, close connection, fd = " + to_string(conn.fd));
        conns.erase(conn.fd);
        close(conn.fd);
    } else {
        modfd(conn.fd, EPOLLET | EPOLLIN);
    }
}

void ET(int listenfd, epoll_event *events, int numbers) {
    string read_buffer(BUFSIZE, '\0');
    for (int i = 0; i < numbers; i++) {
        int fd = events[i].data.fd;
        if (fd == listenfd) {
            sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int connfd = accept(fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
            addfd(connfd);
            conns[connfd] = {connfd, output};
            string client_ip(INET_ADDRSTRLEN, '\0');
            int client_port = ntohs(client_addr.sin_port);
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip.data(), client_ip.size());
            logger(INFO, "connect with " + client_ip + " " + to_string(client_port));
        } else if (events[i].events & EPOLLIN) {
            memset(read_buffer.data(), '\0', read_buffer.size());
            int recvd = 0;
            while (1) {
                int n = recv(fd, read_buffer.data() + recvd, BUFSIZE - recvd, 0);
                if (n > 0) {
                    recvd += n;
                } else if (n < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        logger(INFO, "fd = " + to_string(fd) + " read data:" + read_buffer.substr(0, recvd));
                        logger(DEBUG, "read drained, wait for next EPOLLIN");
                        break;
                    }
                    logger(ERROR, "read failed");
                    close(fd);
                    conns.erase(fd);
                    break;
                } else {
                    if (conns[fd].sendComplete) {
                        logger(INFO, "fd = " + to_string(fd) + " send complete and client finished sending, close connect");
                        close(fd);
                        conns.erase(fd);
                        break;
                    }
                    logger(INFO, "fd = " + to_string(fd) + " client finished sending(FIN received)");
                    conns[fd].readClosed = true;
                    break;
                }
            }
            if (conns.find(fd) != conns.end()) {
                trySend(conns[fd]);
            }
        } else if (events[i].events & EPOLLOUT) {
            if (conns.find(fd) == conns.end()) {
                continue;
            }
            trySend(conns[fd]);
        } else {
            logger(WARN, "something else happened");
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc <= 2) {
        logger(WARN, "argv is too little");
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
    bind(listenfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(listenfd, 1024);
    logger(INFO, "socket success, is listening");

    epoll_event events[MAXSIZE];
    addfd(listenfd);

    while (1) {
        int numbers = epoll_wait(epollfd, events, MAXSIZE, -1);
        if (numbers < 0) {
            logger(ERROR, "epoll_wait failed");
            break;
        }
        ET(listenfd, events, numbers);
    }
    close(epollfd);
    close(listenfd);
    return 0;
}