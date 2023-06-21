
//g++ simple_test.cpp  -I/root/learing/incubator-brpc/output/include   -L/root/learing/incubator-brpc/output/lib -lbrpc  -o your_program_new  -std=c++11
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <chrono>

#include <butil/containers/doubly_buffered_data.h>

struct Config {
    int value;
};

butil::DoublyBufferedData<Config> config_data;

std::atomic<bool> stop_threads(false);
std::atomic<int> dbd_read_count(0);
std::atomic<int> prodcons_read_count(0);

std::mutex queue_mutex;
std::condition_variable queue_cond;
std::queue<Config> config_queue;

void update_config_thread() {
    int counter = 0;
    while (!stop_threads) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto modifier = [&counter](Config& config) -> size_t {
            config.value = counter++;
            std::cout << "update_config_thread Config value: " << config.value << std::endl;
            return 1;
        };
        config_data.Modify(modifier);
    }
}

void read_config_thread() {
    while (!stop_threads) {
        butil::DoublyBufferedData<Config>::ScopedPtr config_ptr;
        if (!config_data.Read(&config_ptr)) {
            dbd_read_count++;
            std::cout << "Config value: " << config_ptr->value << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

void producer_thread() {
    int counter = 0;
    while (!stop_threads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> lock(queue_mutex);
        config_queue.push(Config{counter++});
        queue_cond.notify_one();
    }
}

void consumer_thread() {
    while (!stop_threads) {
        std::unique_lock<std::mutex> lock(queue_mutex);
        queue_cond.wait(lock, [] { return !config_queue.empty() || stop_threads; });
        if (!config_queue.empty()) {
            config_queue.pop();
            prodcons_read_count++;
        }
    }
}

bool AddN(Config& f, int n) {
    f.value += n;
    return true;
}

int main() {
    config_data.Modify(AddN,11);

    // 启动生产者-消费者线程
    std::thread producer(producer_thread);
    std::vector<std::thread> consumers;
    for (int i = 0; i < 100; ++i) {
        consumers.push_back(std::thread(consumer_thread));
    }

    // 启动 DoublyBufferedData 更新和读取配置的线程
    std::thread dbd_updater(update_config_thread);
    std::vector<std::thread> dbd_readers;
    for (int i = 0; i < 100; ++i) {
        dbd_readers.push_back(std::thread(read_config_thread));
    }

    // 等待一段时间后停止线程
    std::this_thread::sleep_for(std::chrono::seconds(5));
    stop_threads = true;
    queue_cond.notify_all(); // 唤醒所有等待的消费者线程

    // 等待线程结束
    producer.join();
    for (auto& t : consumers) {
        t.join();
    }

    dbd_updater.join();
    for (auto& t : dbd_readers) {
        t.join();
    }
 

    auto total_dbd_reads = dbd_read_count.load();
    auto total_prodcons_reads = prodcons_read_count.load();
    std::cout << "Total DoublyBufferedData reads: " << total_dbd_reads << std::endl;
    std::cout << "Total Producer-Consumer reads: " << total_prodcons_reads << std::endl;

    return 0;
}
