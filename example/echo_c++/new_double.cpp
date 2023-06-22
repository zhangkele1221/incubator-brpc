#include <iostream>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>
#include <memory>

template<typename DataType, typename ThreadLocalDataType = int>
class DoublyBufferedData {
public:
    DoublyBufferedData() : frontData(std::make_shared<DataType>()), backData(std::make_shared<DataType>()) {}

    class ScopedPtr {
    public:
        ScopedPtr(std::shared_ptr<DataType> data, std::mutex& mtx) : data(data), lock(mtx) {}
        DataType* operator->() const { return data.get(); }

    private:
        std::shared_ptr<DataType> data;
        std::lock_guard<std::mutex> lock;
    };

    ScopedPtr Read() {
        std::lock_guard<std::mutex> lock(mutex);
        return ScopedPtr(frontData, mutex);
    }

    void Modify(std::function<void(DataType&)> modifyFunc) {
        std::lock_guard<std::mutex> dataLock(dataMutex);
        std::lock_guard<std::mutex> lock(mutex);

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
        return localData.local();
    }

    void AddWrapper(std::mutex& mtx) {
        std::lock_guard<std::mutex> lock(mutex);
        wrapperMutexes.push_back(mtx);
    }

    void RemoveWrapper(std::mutex& mtx) {
        std::lock_guard<std::mutex> lock(mutex);
        wrapperMutexes.erase(std::remove(wrapperMutexes.begin(), wrapperMutexes.end(), mtx), wrapperMutexes.end());
    }

private:
    std::shared_ptr<DataType> frontData;
    std::shared_ptr<DataType> backData;
    std::mutex dataMutex;
    std::mutex mutex;
    std::vector<std::mutex> wrapperMutexes;
    thread_local ThreadLocalDataType localDataStorage;
    std::reference_wrapper<thread_local ThreadLocalDataType> localData{localDataStorage};
};

int main() {
    DoublyBufferedData<int> data;

    // Example: Reading data
    auto readData = data.Read();
    std::cout << "Read data: " << *readData << std::endl;

    // Example: Modifying data
    data.Modify([](int& x) {
        x = 5;
    });

    readData = data.Read();
    std::cout << "Read data after modification: " << *readData << std::endl;

    return 0;
}
