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

int main(int argc, char *argv[]) {
    if (argc <= 2) {
        logger(WARN, "argv is too little");
        return -1;
    }
    string ip(argv[1]);
    int port = stoi(argv[2]);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    int fd = socket(PF_INET, SOCK_STREAM, 0);

    connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    logger(INFO, "connect success");

    string buffer = "GET / HTTP/1.1\r\n"
                    "Host: 127.0.0.1:8080\r\n"
                    "\r\n";
    while (!buffer.empty()) {
        send(fd, buffer.c_str(), 5, 0);
        logger(INFO, "write success, data is:" + buffer.substr(0, 5));
        buffer.erase(0, 5);
    }

    string inbuf(BUFSIZE, '\0');
    recv(fd, inbuf.data(), BUFSIZE, 0);
    logger(INFO, "read data is " + inbuf);

    close(fd);
    return 0;
}