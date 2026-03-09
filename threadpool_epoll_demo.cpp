#include <arpa/inet.h>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <queue>
#include <sstream>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>
using namespace std;

#define MAXSIZE 1024
#define BUFSIZE 4096

string body = "Hello";
string default_response = "HTTP/1.1 200 OK\r\nContent-Length: " + to_string(body.size()) + "\r\n\r\n" + body;
string error_response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";

int epollfd;
int notifyfd;

struct Connection {
    int fd;
    string inbuf;
    string outbuf;
    bool readClosed = false;
    bool sendComplete = false;
    bool processing = false;
};
unordered_map<int, Connection> conns;

struct TaskResult {
    int fd;
    string response;

    TaskResult(int f, string &r) : fd(f), response(r) {}
};
queue<TaskResult> readyQueue;
mutex readyMutex;
mutex logMutex;

// 日志

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
    lock_guard<mutex> lock(logMutex);
    if (level == ERROR || level == FATAL) {
        cout << getTimestamp() << " " << level << ": " << message << " : " << strerror(errno) << endl;
    } else {
        cout << getTimestamp() << " " << level << ": " << message << endl;
    }
}

// 解析http请求状态机

enum ParseState {
    PARSE_REQUEST_LINE,
    PARSE_HEADERS,
    PARSE_BODY,
    PARSE_DONE,
    PARSE_ERROR
};

struct HttpRequest {
    ParseState state = PARSE_REQUEST_LINE;

    string method;
    string target;
    string version;

    string host;
    string content_type;
    int content_length = 0;

    string body;
};

ParseState parse_http_from_string(const string &raw, HttpRequest &req) {
    int index = 0;
    size_t pos;
    string line;
    while (req.state == PARSE_BODY || (pos = raw.find("\r\n", index)) != string::npos) {
        if (req.state == PARSE_REQUEST_LINE || req.state == PARSE_HEADERS) {
            line = raw.substr(index, pos - index);
            index = pos + 2;
        }
        switch (req.state) {
        case PARSE_REQUEST_LINE: {
            istringstream iss(line);
            iss >> req.method >> req.target >> req.version;
            if (req.method.empty() || req.target.empty() || req.version.empty()) {
                logger(DEBUG, "request_line parse error");
                req.state = PARSE_ERROR;
                break;
            }
            if (req.method != "GET" && req.method != "POST" && req.method != "HEAD") {
                logger(DEBUG, "request method error, method is " + req.method);
                req.state = PARSE_ERROR;
                break;
            }
            req.state = PARSE_HEADERS;
            break;
        }

        case PARSE_HEADERS: {
            if (line.empty()) {
                if (req.content_length > 0) {
                    req.state = PARSE_BODY;
                } else {
                    req.state = PARSE_DONE;
                }
                break;
            }

            size_t colon_pos = line.find(':');
            if (colon_pos == string::npos) {
                logger(DEBUG, "headers parse error");
                req.state = PARSE_ERROR;
                break;
            }
            string key = line.substr(0, colon_pos);
            string value = line.substr(colon_pos + 2);

            if (key.empty() || value.empty()) {
                logger(DEBUG, "headers parse error");
                req.state = PARSE_ERROR;
            } else if (key == "Host") {
                req.host = value;
            } else if (key == "Content-Type") {
                req.content_type = value;
            } else if (key == "Content-Length") {
                req.content_length = stoi(value);
            }
            break;
        }

        case PARSE_BODY: {
            req.body = raw.substr(index, req.content_length);
            req.state = PARSE_DONE;
            return PARSE_DONE;
        }

        case PARSE_DONE:
            return PARSE_DONE;

        case PARSE_ERROR:
            return PARSE_ERROR;
        }
    }
    return req.state;
}

// 线程池

template <typename T>
class ThreadPool {
  public:
    ThreadPool(int threadnum = 8, int max_requests = 10000) : m_max_requests(max_requests), m_stop(false) {
        for (int i = 0; i < threadnum; i++) {
            threads.emplace_back([this] { worker(); });
        }
    }

    ~ThreadPool() {
        {
            lock_guard<mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cond.notify_all();
        for (auto &t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    bool enqueue(T *request) {
        {
            lock_guard<mutex> lock(m_mutex);
            if (m_workqueue.size() >= m_max_requests) {
                return false;
            }
            m_workqueue.emplace(request);
        }
        m_cond.notify_one();
        return true;
    }

  private:
    void worker() {
        while (1) {
            T *request = nullptr;
            {
                unique_lock<mutex> lock(m_mutex);
                m_cond.wait(lock, [this] {
                    return m_stop || !m_workqueue.empty();
                });

                if (m_stop && m_workqueue.empty()) {
                    return;
                }

                request = m_workqueue.front();
                m_workqueue.pop();
            }
            if (request) {
                request->process();
            }
        }
    }

  private:
    int m_max_requests;
    vector<thread> threads;
    mutex m_mutex;
    condition_variable m_cond;
    queue<T *> m_workqueue;
    bool m_stop;
};

// 任务类

class Task {
  public:
    Task(int fd) : m_fd(fd) {}

    void process() {
        logger(INFO, "Task processing fd = " + to_string(m_fd) + " by thread " + to_string(pthread_self()));

        // 模拟耗时操作
        // this_thread::sleep_for(chrono::milliseconds(500));

        string response = default_response;
        {
            lock_guard<mutex> lock(readyMutex);
            if (conns.find(m_fd) != conns.end()) {
                logger(INFO, "fd = " + to_string(m_fd) + " send data : " + conns[m_fd].inbuf);
                HttpRequest req;
                ParseState ret = parse_http_from_string(conns[m_fd].inbuf, req);
                if (ret == PARSE_ERROR) {
                    logger(INFO, "HTTP parse failed, fd = " + to_string(m_fd));
                    response = error_response;
                } else if (req.target == "/echo") {
                    response = "HTTP/1.1 200 OK\r\nContent-Length: " + to_string(body.size()) + "\r\n\r\n" + req.body;
                } else if (req.method == "GET") {
                    string file_path = "static" + req.target;
                    if (req.target == "/") file_path = "static/index.html";
                    logger(INFO, "file path is " + file_path);
                    int filefd = open(file_path.c_str(), O_RDONLY);
                    if (filefd == -1) {
                        response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
                    } else {
                        struct stat st;
                        fstat(filefd, &st);

                        long file_size = st.st_size;
                        string body(file_size, '\0');
                        read(filefd, body.data(), file_size);
                        close(filefd);
                        response = "HTTP/1.1 200 OK\r\nContent-Length: " + to_string(file_size) + "\r\n\r\n" + body;
                    }
                }
                readyQueue.emplace(m_fd, response);
            }
        }

        uint64_t val = 1;
        write(notifyfd, &val, sizeof(val));
    }

  private:
    int m_fd;
};

// 网络编程

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

void modfd(int fd, uint32_t events) {
    epoll_event event;
    event.events = events;
    event.data.fd = fd;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

void trySend(Connection &conn) {
    if (conn.outbuf.empty()) {
        return;
    }
    while (!conn.outbuf.empty()) {
        int n = send(conn.fd, conn.outbuf.c_str(), conn.outbuf.size(), 0);
        if (n > 0) {
            conn.outbuf.erase(0, n);
        } else if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            logger(DEBUG, "write would block, enable EPOLLOUT and send later");
            modfd(conn.fd, EPOLLET | EPOLLIN | EPOLLOUT);
            return;
        } else {
            logger(ERROR, "write failed");
            return;
        }
    }
    logger(DEBUG, "fd = " + to_string(conn.fd) + " send over");
    conn.sendComplete = true;
    if (conn.readClosed) {
        logger(INFO, "send complete, close fd = " + to_string(conn.fd));
        close(conn.fd);
    } else {
        logger(INFO, "send complete, close fd = " + to_string(conn.fd));
        close(conn.fd);
    }
}

int main(int argc, char *argv[]) {
    if (argc <= 2) {
        logger(WARN, "usage: ./threadpool_epoll_demo.out ip port");
        return -1;
    }
    epollfd = epoll_create(1);
    notifyfd = eventfd(0, EFD_NONBLOCK);
    addfd(notifyfd);

    string ip(argv[1]);
    int port = stoi(argv[2]);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind(listenfd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(listenfd, 1024);
    addfd(listenfd);
    logger(INFO, "server listening on " + ip + ":" + to_string(port));

    epoll_event events[MAXSIZE];

    ThreadPool<Task> pool(4);

    while (1) {
        int numbers = epoll_wait(epollfd, events, MAXSIZE, -1);
        logger(DEBUG, "happened events number = " + to_string(numbers));
        if (numbers < 0) {
            logger(ERROR, "epoll_wait failed");
            break;
        }
        for (int i = 0; i < numbers; i++) {
            int fd = events[i].data.fd;
            logger(DEBUG, "event fd = " + to_string(fd));
            if (fd == listenfd) {
                while (1) {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int connfd = accept(fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
                    if (connfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        logger(ERROR, "accept failed");
                        break;
                    }

                    addfd(connfd);
                    conns[connfd] = {connfd, "", "", false, false, false};
                    string client_ip;
                    int client_port = ntohs(client_addr.sin_port);
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip.data(), client_ip.size());
                    logger(INFO, "new connection, fd = " + to_string(connfd) + " ip = " + client_ip + ":" + to_string(client_port));
                }
            } else if (fd == notifyfd) {
                uint64_t val;
                read(notifyfd, &val, sizeof(val));

                queue<TaskResult> localQueue;
                {
                    lock_guard<mutex> lock(readyMutex);
                    localQueue.swap(readyQueue);
                }

                logger(DEBUG, "start processing queue, queue len = " + to_string(localQueue.size()));
                while (!localQueue.empty()) {
                    auto &result = localQueue.front();
                    if (conns.find(result.fd) != conns.end()) {
                        conns[result.fd].outbuf = result.response;
                        conns[result.fd].processing = false;
                        trySend(conns[result.fd]);
                    }
                    localQueue.pop();
                }
            } else if (events[i].events & EPOLLIN) {
                char buf[BUFSIZE];
                while (1) {
                    int n = recv(fd, buf, BUFSIZE, 0);
                    if (n > 0) {
                        conns[fd].inbuf.append(buf, n);
                    } else if (n == 0) {
                        logger(INFO, "client closed reading, fd = " + to_string(fd));
                        conns[fd].readClosed = true;
                        if (conns[fd].sendComplete) {
                            close(fd);
                        }
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            if (!conns[fd].processing && !conns[fd].sendComplete) {
                                conns[fd].processing = true;
                                logger(DEBUG, "enqueue task for fd = " + to_string(fd));
                                pool.enqueue(new Task(fd));
                            }
                            break;
                        } else {
                            logger(ERROR, "read failed");
                            close(fd);
                            break;
                        }
                    }
                }
            } else if (events[i].events & EPOLLOUT) {
                if (conns.find(fd) != conns.end()) {
                    trySend(conns[fd]);
                }
            } else {
                logger(WARN, "something else happened");
            }
        }
    }
}