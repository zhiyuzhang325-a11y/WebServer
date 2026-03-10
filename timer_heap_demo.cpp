#include <cstring>
#include <ctime>
#include <iostream>
#include <vector>

using namespace std;

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

struct Timer {
    int fd;
    int expire_time;
};

class TimerHeap {
  public:
    TimerHeap(vector<Timer> &timers) : m_timers(timers), m_size(timers.size()) {
        for (int i = (m_size - 2) / 2; i >= 0; i--) {
            siftDown(i);
        }
    }

    void add(Timer &timer) {
        m_timers.push_back(timer);
        siftUp(m_size);
        m_size++;
    }

    void tick() {
        while (m_size > 0 && time(nullptr) > m_timers[0].expire_time) {
            swap(m_timers[0], m_timers[m_size - 1]);
            m_timers.pop_back();
            m_size--;
            siftDown(0);
        }
    }

    int getNextTimeout() {
        if (m_size == 0) {
            logger(WARN, "no timer");
            return -1;
        }
        return m_timers[0].expire_time - time(nullptr);
    }

  private:
    void siftDown(int index) {
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
            index = smallest;
        }
    }

    void siftUp(int index) {
        while (1) {
            int father = (index - 1) / 2;
            if (m_timers[father].expire_time > m_timers[index].expire_time) {
                swap(m_timers[father], m_timers[index]);
                index = father;
            } else {
                break;
            }
        }
    }

  private:
    int m_size;
    vector<Timer> m_timers;
};