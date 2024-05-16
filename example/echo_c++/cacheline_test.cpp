
//https://brpc.apache.org/zh/docs/rpc-in-depth/atomic-instructions/    对文档中的内容说法合理性 测试      
// 另一个例子是计数器，如果所有线程都频繁修改一个计数器，性能就会很差，原因同样在于不同的核心在不停地同步同一个cacheline。如果这个计数器只是用作打打日志之类的，那我们完全可以让每个线程修改thread-local变量，在需要时再合并所有线程中的值，性能可能有几十倍的差别。
// 确实是几十倍的 差距
#include <atomic>
#include <thread>
#include <vector>
#include <iostream>
#include <chrono>
#include <fstream>

std::atomic<int> global_counter(0);
thread_local int local_counter = 0;
std::vector<int> global_counters(4);

void increment_global() {
    for (int i = 0; i < 1000000; ++i) {
        global_counter.fetch_add(1, std::memory_order_relaxed);
    }
}

void increment_thread_local(int thread_id) {
    for (int i = 0; i < 1000000; ++i) {
        ++local_counter;
    }
    global_counters[thread_id] = local_counter;
}

void test_global_counter(std::ofstream& outfile) {
    global_counter.store(0);
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(increment_global);
    }
    for (auto& thread : threads) {
        thread.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;

    outfile << "Global counter: " << global_counter.load() << std::endl;
    outfile << "Time taken (global counter): " << duration.count() << " seconds" << std::endl;
}

void test_thread_local_counter(std::ofstream& outfile) {
    global_counters.assign(4, 0);
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back(increment_thread_local, i);
    }
    for (auto& thread : threads) {
        thread.join();
    }

    int total = 0;
    for (int counter : global_counters) {
        total += counter;
    }

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> duration = end - start;

    outfile << "Total counter: " << total << std::endl;
    outfile << "Time taken (thread-local counter): " << duration.count() << " seconds" << std::endl;
}

int main() {
    std::ofstream outfile("test_results.txt", std::ios::app);

    outfile << "Testing global counter:" << std::endl;
    test_global_counter(outfile);

    outfile << "Testing thread-local counter:" << std::endl;
    test_thread_local_counter(outfile);

    outfile.close();
    return 0;
}
