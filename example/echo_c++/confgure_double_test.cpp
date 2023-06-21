//g++ confgure_double_test.cpp  -I/root/learing/incubator-brpc/output/include   -L/root/learing/incubator-brpc/output/lib -lbrpc  -o your_program_new  -std=c++11

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <chrono>

#include <butil/containers/doubly_buffered_data.h>

struct Config {
    int value;
};

butil::DoublyBufferedData<Config> config_data;

std::atomic<bool> stop_threads(false);
std::atomic<int> read_count(0);

// 模拟后台线程，定期更新配置数据
void update_config_thread() {
    int counter = 0;
    while (!stop_threads) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto modifier = [&counter](Config& config) -> size_t {
            config.value = counter++;
            std::cout << "update_config_thread Config value: " << config.value << std::endl;
            return 1;  // 指示已进行修改
        };

        config_data.Modify(modifier);
    }
}

// 模拟工作线程，读取配置数据
void read_config_thread() {
    while (!stop_threads) {
        butil::DoublyBufferedData<Config>::ScopedPtr config_ptr;
        if (!config_data.Read(&config_ptr)) {
            std::cout << "Config value: " << config_ptr->value << std::endl;
            read_count++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

bool AddN(Config& f, int n) {
    f.value += n;
    return true;
}

int main() {
    config_data.Modify(AddN,10);

    // 创建并运行线程
    std::thread updater(update_config_thread);
    std::vector<std::thread> readers;
    const int num_readers = 5; // 消费者线程数

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_readers; ++i) {
        readers.push_back(std::thread(read_config_thread));
    }

    // 等待一段时间后停止线程
    std::this_thread::sleep_for(std::chrono::seconds(5));
    stop_threads = true;

    // 等待线程结束
    updater.join();
    for (auto& t : readers) {
        t.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    std::cout << "Reads per second: " << (read_count.load() * 1000) / duration << std::endl;

    return 0;
}
