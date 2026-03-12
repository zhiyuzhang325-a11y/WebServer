#include <arpa/inet.h>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <functional>
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
    bool keepAlive = true;
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

class AsyncLogger {
  public:
    enum LOGLEVEL {
        DEBUG, // 调试
        INFO,  // 信息
        WARN,  // 警告
        ERROR, // 错误
        FATAL  // 致命
    };

    AsyncLogger(int flush_threshold) : m_flush_threshold(flush_threshold), m_stop(false) {
        m_file.open("logs/server.log", ios::app);
        if (!m_file.is_open()) {
            std::cerr << "log file open failed!" << std::endl;
        }

        m_backend = thread(&AsyncLogger::backend, this);
    }

    void log(LOGLEVEL level, const string &message) {
        string entry = getTimestamp() + " " + levelToString(level) + " " + message;
        if (level == ERROR) {
            entry.append(": " + string(strerror(errno)));
        }

        {
            lock_guard<mutex> lock(m_mutex);
            m_buffer_a.push(entry);
            if (m_buffer_a.size() > m_flush_threshold) {
                m_cond.notify_one();
            }
        }
    }

    ~AsyncLogger() {
        {
            lock_guard<mutex> lock(m_mutex);
            m_stop = true;
        }

        m_cond.notify_one();
        if (m_backend.joinable()) {
            m_backend.join();
        }
    }

  private:
    string getTimestamp() {
        auto now = chrono::system_clock::now();
        auto ms = chrono::duration_cast<chrono::milliseconds>(now.time_since_epoch()) % 1000;
        time_t t = chrono::system_clock::to_time_t(now);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&t));
        return string(buf) + "." + to_string(ms.count());
    }

    string levelToString(LOGLEVEL level) {
        switch (level) {
        case DEBUG:
            return "DEBUG";
        case INFO:
            return "INFO";
        case WARN:
            return "WARN";
        case ERROR:
            return "ERROR";
        case FATAL:
            return "FATAL";
        default:
            return "UNKNOWN";
        }
    }

    void backend() {
        while (1) {
            {
                unique_lock<mutex> lock(m_mutex);
                m_cond.wait_for(lock, std::chrono::seconds(3), [this] { return m_stop || m_buffer_a.size() >= m_flush_threshold; });

                m_buffer_b.swap(m_buffer_a);
            }

            while (!m_buffer_b.empty()) {
                m_file << m_buffer_b.front() << '\n';
                m_buffer_b.pop();
            }
            m_file.flush();

            if (m_stop) {
                return;
            }
        }
    }

  private:
    queue<string> m_buffer_a;
    queue<string> m_buffer_b;
    mutex m_mutex;
    condition_variable m_cond;
    thread m_backend;
    bool m_stop = false;
    int m_flush_threshold;
    ofstream m_file;
};

AsyncLogger logger(5);

// 时间堆

struct Timer {
    int fd;
    long long expire_time;
};

class TimerHeap {
  public:
    TimerHeap(vector<Timer> &timers) : m_timers(timers), m_size(timers.size()) {
        for (int i = (m_size - 2) / 2; i >= 0; i--) {
            siftDown(i);
        }
        for (int i = 0; i < m_size; i++) {
            m_timers_index[m_timers[i].fd] = i;
        }
    }

    TimerHeap() : m_size(0) {}

    void add(const Timer &timer) {
        m_timers.push_back(timer);
        siftUp(m_size);
        m_size++;
    }

    void tick(function<void(int)> on_expire) {
        while (m_size > 0 && time(nullptr) * 1000ll >= m_timers[0].expire_time) {
            logger.log(AsyncLogger::DEBUG, "tick: now=" + to_string(time(nullptr) * 1000ll) + " expire=" + to_string(m_timers[0].expire_time));
            on_expire(m_timers[0].fd);
            swap(m_timers[0], m_timers[m_size - 1]);
            m_timers.pop_back();
            m_size--;
            siftDown(0);
        }
    }

    int getNextTimeout() {
        tick([](int fd) {
            logger.log(AsyncLogger::INFO, "close inactive connection, fd = " + to_string(fd));
            close(fd);
        });

        if (m_size == 0) {
            logger.log(AsyncLogger::WARN, "no timer");
            return -1;
        }
        int timeout = m_timers[0].expire_time - time(nullptr) * 1000;
        logger.log(AsyncLogger::DEBUG, "timeout = " + to_string(timeout));
        return timeout;
    }

    void update(int fd, long long update_time) {
        int index = m_timers_index[fd];
        m_timers[index].expire_time = update_time;
        siftDown(index);
    }

    void remove(int fd) {
        int index = m_timers_index[fd];
        swap(m_timers[index], m_timers[m_size - 1]);
        m_timers_index.erase(fd);
        m_timers.pop_back();
        m_size--;
        if (index < m_size) {
            index = siftDown(index);
            siftUp(index);
        }
    }

  private:
    int siftDown(int index) {
        while (1) {
            int left_child = 2 * index + 1;
            int right_child = 2 * index + 2;
            int smallest = index;

            if (left_child < m_size && m_timers[left_child].expire_time < m_timers[smallest].expire_time) {
                smallest = left_child;
            }

            if (right_child < m_size && m_timers[right_child].expire_time < m_timers[smallest].expire_time) {
                smallest = right_child;
            }

            if (smallest == index) {
                break;
            }

            swap(m_timers[index], m_timers[smallest]);
            m_timers_index[m_timers[smallest].fd] = index;
            index = smallest;
        }
        m_timers_index[m_timers[index].fd] = index;
        return index;
    }

    int siftUp(int index) {
        while (1) {
            int father = (index - 1) / 2;
            if (m_timers[father].expire_time > m_timers[index].expire_time) {
                swap(m_timers[father], m_timers[index]);
                m_timers_index[m_timers[father].fd] = index;
                index = father;
            } else {
                break;
            }
        }
        m_timers_index[m_timers[index].fd] = index;
        return index;
    }

  private:
    int m_size;
    vector<Timer> m_timers;
    unordered_map<int, int> m_timers_index;
};

TimerHeap heap;

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
    string connection;
    string content_type;
    int content_length = 0;

    string body;

    size_t end_pos;
};

ParseState parse_http_from_string(const string &raw, HttpRequest &req) {
    size_t index = 0;
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
                logger.log(AsyncLogger::DEBUG, "request_line parse error");
                req.state = PARSE_ERROR;
                break;
            }
            if (req.method != "GET" && req.method != "POST" && req.method != "HEAD") {
                logger.log(AsyncLogger::DEBUG, "request method error, method is " + req.method);
                req.state = PARSE_ERROR;
                break;
            }
            req.state = PARSE_HEADERS;
            break;
        }

        case PARSE_HEADERS: {
            if (line.empty()) {
                req.end_pos = index + req.content_length;
                if (req.content_length > 0) {
                    req.state = PARSE_BODY;
                } else {
                    req.state = PARSE_DONE;
                }
                break;
            }

            size_t colon_pos = line.find(':');
            if (colon_pos == string::npos) {
                logger.log(AsyncLogger::DEBUG, "headers parse error");
                req.state = PARSE_ERROR;
                break;
            }
            string key = line.substr(0, colon_pos);
            string value = line.substr(colon_pos + 2);

            transform(key.begin(), key.end(), key.begin(), ::tolower);

            if (key.empty() || value.empty()) {
                logger.log(AsyncLogger::DEBUG, "headers parse error");
                req.state = PARSE_ERROR;
            } else if (key == "host") {
                req.host = value;
            } else if (key == "connection") {
                transform(value.begin(), value.end(), value.begin(), ::tolower);
                req.connection = value;
            } else if (key == "content-type") {
                req.content_type = value;
            } else if (key == "content-length") {
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
        logger.log(AsyncLogger::INFO, "Task processing fd = " + to_string(m_fd) + " by thread " + to_string(pthread_self()));

        // 模拟耗时操作
        // this_thread::sleep_for(chrono::milliseconds(500));

        string response = default_response;
        {
            lock_guard<mutex> lock(readyMutex);
            if (conns.find(m_fd) != conns.end()) {
                logger.log(AsyncLogger::INFO, "fd = " + to_string(m_fd) + " send data : " + conns[m_fd].inbuf);
                HttpRequest req;
                ParseState ret = parse_http_from_string(conns[m_fd].inbuf, req);
                if (ret == PARSE_ERROR) {
                    logger.log(AsyncLogger::INFO, "HTTP parse failed, fd = " + to_string(m_fd));
                    response = error_response;
                } else if (req.target == "/echo") {
                    response = "HTTP/1.1 200 OK\r\nContent-Length: " + to_string(req.body.size()) + "\r\n\r\n" + req.body;
                } else if (req.method == "GET") {
                    string file_path = "static" + req.target;
                    if (req.target == "/") file_path = "static/index.html";
                    logger.log(AsyncLogger::INFO, "file path is " + file_path);
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
                conns[m_fd].inbuf.erase(conns[m_fd].inbuf.begin(), conns[m_fd].inbuf.begin() + req.end_pos);

                if (req.version == "HTTP/1.0" || req.connection == "close") {
                    conns[m_fd].keepAlive = false;
                }
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
            logger.log(AsyncLogger::DEBUG, "write would block, enable EPOLLOUT and send later");
            modfd(conn.fd, EPOLLET | EPOLLIN | EPOLLOUT);
            return;
        } else {
            logger.log(AsyncLogger::ERROR, "write failed");
            return;
        }
    }
    conn.sendComplete = true;
    if (conn.readClosed) {
        logger.log(AsyncLogger::INFO, "send complete, close fd = " + to_string(conn.fd));
        heap.remove(conn.fd);
        close(conn.fd);
    } else if (!conn.keepAlive) {
        logger.log(AsyncLogger::INFO, "client not keep alive, send complete, close fd = " + to_string(conn.fd));
        heap.remove(conn.fd);
        close(conn.fd);
    } else {
        logger.log(AsyncLogger::INFO, "fd = " + to_string(conn.fd) + ", send complete");
        conn.sendComplete = false;
    }
}

int main(int argc, char *argv[]) {
    logger.log(AsyncLogger::INFO, "\nserver starting");

    if (argc <= 2) {
        logger.log(AsyncLogger::WARN, "usage: ./threadpool_epoll_demo.out ip port");
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
    logger.log(AsyncLogger::INFO, "server listening on " + ip + ":" + to_string(port));

    epoll_event events[MAXSIZE];

    ThreadPool<Task> pool(4);

    while (1) {
        int numbers = epoll_wait(epollfd, events, MAXSIZE, heap.getNextTimeout());
        logger.log(AsyncLogger::DEBUG, "happened events number = " + to_string(numbers));
        if (numbers < 0) {
            logger.log(AsyncLogger::ERROR, "epoll_wait failed");
            break;
        } else if (numbers == 0) {
            logger.log(AsyncLogger::INFO, "epoll_wait running timeout, run timer tick");
            heap.tick([](int fd) {
                logger.log(AsyncLogger::INFO, "close inactive connection, fd = " + to_string(fd));
                close(fd);
            });
            continue;
        }
        for (int i = 0; i < numbers; i++) {
            int fd = events[i].data.fd;
            logger.log(AsyncLogger::DEBUG, "event fd = " + to_string(fd));
            if (fd == listenfd) {
                while (1) {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int connfd = accept(fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
                    if (connfd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        }
                        logger.log(AsyncLogger::ERROR, "accept failed");
                        break;
                    }

                    addfd(connfd);
                    conns[connfd] = {connfd, "", "", false, false, false, true};
                    heap.add(Timer{connfd, (time(nullptr) * 1000ll + 6000)});
                    string client_ip;
                    int client_port = ntohs(client_addr.sin_port);
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip.data(), client_ip.size());
                    logger.log(AsyncLogger::INFO, "new connection, fd = " + to_string(connfd) + " ip = " + client_ip + ":" + to_string(client_port));
                }
            } else if (fd == notifyfd) {
                uint64_t val;
                read(notifyfd, &val, sizeof(val));

                queue<TaskResult> localQueue;
                {
                    lock_guard<mutex> lock(readyMutex);
                    localQueue.swap(readyQueue);
                }

                logger.log(AsyncLogger::DEBUG, "start processing queue, queue len = " + to_string(localQueue.size()));
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
                heap.update(fd, (time(nullptr) * 1000ll + 6000));

                char buf[BUFSIZE];
                while (1) {
                    int n = recv(fd, buf, BUFSIZE, 0);
                    if (n > 0) {
                        conns[fd].inbuf.append(buf, n);
                    } else if (n == 0) {
                        logger.log(AsyncLogger::INFO, "client closed writing, fd = " + to_string(fd));
                        conns[fd].readClosed = true;
                        if (conns[fd].sendComplete) {
                            logger.log(AsyncLogger::INFO, "and send has completed, close fd = " + to_string(fd));
                            heap.remove(fd);
                            close(fd);
                        }
                        break;
                    } else {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            size_t head_end_pos = conns[fd].inbuf.find("\r\n\r\n");
                            if (head_end_pos != string::npos) {
                                const string key = "Content-Length:";
                                size_t value_pos = conns[fd].inbuf.find(key);
                                if (value_pos != string::npos) {
                                    value_pos += key.size();
                                    value_pos = conns[fd].inbuf.find_first_not_of(' ', value_pos);

                                    if (value_pos != string::npos) {
                                        size_t value_end = conns[fd].inbuf.find("\r\n", value_pos);
                                        int value = stoi(conns[fd].inbuf.substr(value_pos, value_end - value_pos));

                                        size_t body_start_pos = head_end_pos + 4;
                                        size_t body_size = conns[fd].inbuf.size() - body_start_pos;
                                        if (body_size < value) {
                                            logger.log(AsyncLogger::WARN, "HTTP body incomplete, received body size = " + to_string(body_size) + " ,expected = " + to_string(value));
                                            break;
                                        } else {
                                            logger.log(AsyncLogger::DEBUG, "HTTP received complete");
                                        }
                                    } else {
                                        logger.log(AsyncLogger::WARN, "HTTP incomplete or error");
                                        break;
                                    }
                                }
                            }

                            if (!conns[fd].processing && conns[fd].keepAlive) {
                                logger.log(AsyncLogger::DEBUG, "enqueue task for fd = " + to_string(fd));
                                conns[fd].processing = true;
                                pool.enqueue(new Task(fd));
                            }
                            break;
                        } else {
                            logger.log(AsyncLogger::ERROR, "read failed");
                            heap.remove(fd);
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
                logger.log(AsyncLogger::WARN, "something else happened");
            }
        }
    }

    close(listenfd);
    close(epollfd);
    close(notifyfd);

    return 0;
}