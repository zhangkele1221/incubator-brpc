#include <iostream>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <memory>
#include <algorithm>

#include <chrono>
#include <atomic>



template<typename DataType, typename ThreadLocalDataType = int>
class DoublyBufferedData_self {
public:
    DoublyBufferedData_self() : frontData(std::make_shared<DataType>()), backData(std::make_shared<DataType>()) {}

    class ScopedPtr {
    public:
        ScopedPtr(std::shared_ptr<DataType> data, std::unique_lock<std::mutex>&& lock) : data(data), lock(std::move(lock)) {}
        
        // Move constructor
        ScopedPtr(ScopedPtr&& other) : data(std::move(other.data)), lock(std::move(other.lock)) {}

        // Delete copy constructor
        ScopedPtr(const ScopedPtr&) = delete;

        DataType* operator->() const { return data.get(); }

    private:
        std::shared_ptr<DataType> data;
        std::unique_lock<std::mutex> lock;
    };

    ScopedPtr Read() {
        std::unique_lock<std::mutex> lock(mutex);
        return ScopedPtr(frontData, std::move(lock));
    }

    void Modify(std::function<void(DataType&)> modifyFunc) {
        std::unique_lock<std::mutex> dataLock(dataMutex);
        std::unique_lock<std::mutex> lock(mutex);

        modifyFunc(*backData);

        for (auto& wrapperMutex : wrapperMutexes) {
            wrapperMutex.lock();
        }

        std::swap(frontData, backData);

        for (auto& wrapperMutex : wrapperMutexes) {
            wrapperMutex.unlock();
        }

        modifyFunc(*backData);
    }

    ThreadLocalDataType& GetLocalData() {
        return localDataStorage;
    }

    void AddWrapper(std::mutex& mtx) {
        std::unique_lock<std::mutex> lock(mutex);
        wrapperMutexes.push_back(mtx);
    }

    void RemoveWrapper(std::mutex& mtx) {
        std::unique_lock<std::mutex> lock(mutex);
        wrapperMutexes.erase(std::remove(wrapperMutexes.begin(), wrapperMutexes.end(), mtx), wrapperMutexes.end());
    }

private:
    std::shared_ptr<DataType> frontData;
    std::shared_ptr<DataType> backData;
    std::mutex dataMutex;
    std::mutex mutex;
    std::vector<std::mutex> wrapperMutexes;
    static thread_local ThreadLocalDataType localDataStorage;
};

// 需要在类外初始化静态成员变量
template<typename DataType, typename ThreadLocalDataType>
thread_local ThreadLocalDataType DoublyBufferedData_self<DataType, ThreadLocalDataType>::localDataStorage;



int main() {

    DoublyBufferedData_self<int> data;

    // Example: Modifying data
    data.Modify([](int& x) {
        x = 5;
    });

    // Example: Reading data
    auto readData = data.Read();
    std::cout << "Read data after modification: " << *(readData.operator->()) << std::endl;

    return 0;
}

