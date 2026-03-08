#pragma once

#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>
using namespace std;

template <typename T>
class ThreadPool {
  public:
    ThreadPool(int thread_number = 8, int max_requests = 10000) : m_thread_number(thread_number), m_max_requests(max_requests), m_stop(false) {
        for (int i = 0; i < thread_number; i++) {
            m_threads.emplace_back([this] { worker(); });
        }
    }

    ~ThreadPool() {
        {
            lock_guard<mutex> lock(m_mutex);
            m_stop = true;
        }
        m_connd.notify_all();
        for (auto &t : m_threads) {
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
        m_connd.notify_one();
        return true;
    }

  private:
    void worker() {
        while (1) {
            T *request = nullptr;
            {
                unique_lock<mutex> lock(m_mutex);
                m_connd.wait(lock, [this] {
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
    int m_thread_number;
    int m_max_requests;
    vector<thread> m_threads;
    mutex m_mutex;
    condition_variable m_connd;
    queue<T *> m_workqueue;
    bool m_stop;
};

class Task {
  public:
    Task(int id) : m_id(id) {}

    void process() {
        cout << "Task " << m_id << " processed by thread " << this_thread::get_id() << endl;
    }

  private:
    int m_id;
};

int main() {
    ThreadPool<Task> pool(4);
    vector<Task> tasks;
    for (int i = 0; i < 20; i++) {
        tasks.emplace_back(i);
    }

    for (auto &t : tasks) {
        pool.enqueue(&t);
    }

    sleep(30);

    return 0;
}