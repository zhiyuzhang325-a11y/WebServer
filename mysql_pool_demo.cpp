#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <mysql/mysql.h>
#include <queue>
#include <thread>
#include <vector>

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

class MysqlPool;

class ConnGuard {
  public:
    ConnGuard(MysqlPool *pool);
    ~ConnGuard();
    MYSQL *get() {
        return m_fd;
    }

  private:
    MysqlPool *m_pool;
    MYSQL *m_fd;
};

class MysqlPool {
    friend class ConnGuard;

  public:
    MysqlPool(int max_connections) {
        for (int i = 0; i < max_connections; i++) {
            MYSQL *connfd = mysql_init(nullptr);
            if (connfd == nullptr) {
                logger.log(AsyncLogger::ERROR, "mysql_init failed");
                continue;
            }

            MYSQL *ret = mysql_real_connect(connfd, "127.0.0.1", "root", "123456", "webserver", 3306, nullptr, 0);
            if (ret == nullptr) {
                logger.log(AsyncLogger::ERROR, "mysql_real_connect failed");
                mysql_close(connfd);
                continue;
            }

            m_ready_queue.push(connfd);
        }
    }

    unsigned int executeQuery(const string &sql, string &result_text) {
        MYSQL *fd;
        int num_fields;

        {
            ConnGuard guard(this);
            fd = guard.get();

            int ret = mysql_query(fd, sql.c_str());
            if (ret != 0) {
                logger.log(AsyncLogger::ERROR, "mysql_query failed");
                return -1;
            }

            MYSQL_RES *result = mysql_store_result(fd);
            num_fields = mysql_num_fields(result);

            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result)) != nullptr) {
                for (int i = 0; i < num_fields; i++) {
                    if (row[i] != nullptr) {
                        result_text.append(row[i]);
                        result_text.append(" ");
                    } else {
                        result_text.append("NULL ");
                    }
                }
            }

            mysql_free_result(result);
        }

        return num_fields;
    }

    void executeQuery(const string &sql) {
        MYSQL *fd;

        {
            ConnGuard guard(this);
            fd = guard.get();

            int ret = mysql_query(fd, sql.c_str());
            if (ret != 0) {
                logger.log(AsyncLogger::ERROR, "mysql_query failed: " + string(mysql_error(fd)));
                return;
            } else {
                logger.log(AsyncLogger::INFO, "success, affected rows = " + mysql_affected_rows(fd));
            }
        }
    }

  private:
    queue<MYSQL *> m_ready_queue;
    mutex m_mutex;
    condition_variable m_cond;
};

ConnGuard::ConnGuard(MysqlPool *pool) : m_pool(pool) {
    {
        unique_lock<mutex> lock(pool->m_mutex);
        pool->m_cond.wait(lock, [pool] {
            return !pool->m_ready_queue.empty();
        });

        m_fd = pool->m_ready_queue.front();
        pool->m_ready_queue.pop();
    }
}

ConnGuard::~ConnGuard() {
    {
        lock_guard<mutex> lock(m_pool->m_mutex);
        m_pool->m_ready_queue.push(m_fd);
        m_pool->m_cond.notify_one();
    }
}

int main() {
    MysqlPool mysql_pool(5);

    string result;

    mysql_pool.executeQuery("SELECT * FROM users", result);
    cout << result << endl;
    result = "";

    mysql_pool.executeQuery("INSERT INTO users (username, password_hash, salt) VALUES ('xioali', 'hash345', 'salt345')");

    mysql_pool.executeQuery("SELECT * FROM users", result);

    cout << result << endl;
}