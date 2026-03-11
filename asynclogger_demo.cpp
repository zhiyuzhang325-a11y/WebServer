#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>

using namespace std;

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
                m_cond.wait(lock, [this] {
                    return m_stop || m_buffer_a.size() >= m_flush_threshold;
                });

                m_buffer_b.swap(m_buffer_a);
            }

            while (!m_buffer_b.empty()) {
                m_file << m_buffer_b.front() << '\n';
                m_buffer_b.pop();
            }

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

void f() {
    for (int i = 0; i < 10; i++)
        logger.log(AsyncLogger::DEBUG, "this is a log");
}

int main() {
    thread t1(f);
    thread t2(f);
    t1.join();
    t2.join();
}