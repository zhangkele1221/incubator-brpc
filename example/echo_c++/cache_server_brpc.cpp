//curl -d '{"message":"product_123"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'
// ab -n 100 -c 10 -p data.json -T application/json http://localhost:8000/example.EchoService/Echo | jq -r  '.message'

// ab 压测工具
// ab -n 100 -c 20 -T "application/json" -p data.json http://localhost:8000/example.EchoService/Echo
// data.json 文件内容   {"message":"popular_key"}

//观察 brpc_cache_demo.log 



//# 生成100个不同的键
// seq 1 100 | shuf > keys.txt

// # 使用20个并发发送100个请求，每个请求使用不同的键
// cat keys.txt | xargs -I{} -P20 curl -d '{"message":"key_{}"}' http://localhost:8000/example.EchoService/Echo



#include <gflags/gflags.h>
#include <butil/logging.h>
#include <brpc/server.h>
#include <bthread/bthread.h>
#include <pthread.h>
#include <map>
#include <mutex>
#include <unordered_map>
#include <shared_mutex>
#include <set>
#include "echo.pb.h"

DEFINE_bool(echo_attachment, true, "Echo attachment as well");
DEFINE_int32(port, 8000, "TCP Port of this server");
DEFINE_string(listen_addr, "", "Server listen address, may be IPV4/IPV6/UDS."
            " If this is set, the flag port will be ignored");
DEFINE_int32(idle_timeout_s, -1, "Connection will be closed if there is no "
             "read/write operations during the last `idle_timeout_s'");
DEFINE_int32(logoff_ms, 2000, "Maximum duration of server's LOGOFF state "
             "(waiting for client to close connection before server stops)");

// 定义全局的键
static bthread_key_t bls_key;
static pthread_key_t tls_key;

// BLS 和 TLS 的析构函数
void bls_destructor(void* data) {
    int value = *(int*)data;
    LOG(INFO) << "销毁BLS数据: bthread=" << bthread_self()
              << ", 最终值=" << value;
    delete (int*)data;
}

void tls_destructor(void* data) {
    int value = *(int*)data;
    LOG(INFO) << "销毁TLS数据: pthread=" << pthread_self()
              << ", 最终值=" << value;
    delete (int*)data;
}

// 初始化键
void init_keys() {
    CHECK_EQ(0, bthread_key_create(&bls_key, bls_destructor));
    CHECK_EQ(0, pthread_key_create(&tls_key, tls_destructor));
}

// 获取或创建当前 BLS 的数据
int* get_or_create_bls_data() {
    void* data = bthread_getspecific(bls_key);
    if (!data) {
        LOG(INFO) << "创建新的BLS数据: bthread=" << bthread_self();
        data = new int(0);
        CHECK_EQ(0, bthread_setspecific(bls_key, data));
    }
    return (int*)data;
}

// 获取或创建当前 TLS 的数据
int* get_or_create_tls_data() {
    void* data = pthread_getspecific(tls_key);
    if (!data) {
        LOG(INFO) << "创建新的TLS数据: pthread=" << pthread_self();
        data = new int(0);
        CHECK_EQ(0, pthread_setspecific(tls_key, data));
    }
    return (int*)data;
}

namespace example {

// ====================== 缓存实现 ======================

// 错误实现：全局缓存（无锁）
class UnsafeGlobalCache {
public:
    // 获取缓存值（线程不安全）
    std::string get(const std::string& key) {
        LOG(INFO) << "[UnsafeCache] 访问键: " << key;
        
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            LOG(INFO) << "[UnsafeCache] 缓存命中: " << key;
            return it->second;
        }
        
        LOG(WARNING) << "[UnsafeCache] 缓存未命中: " << key << " (开始计算新值)";
        usleep(5000); // 5ms延迟
        
        // 生成新值并缓存
        std::string value = "Value for " + key;
        cache_[key] = value;
        
        LOG(INFO) << "[UnsafeCache] 添加新缓存: " << key << " = " << value;
        return value;
    }
    
    size_t size() const { 
        LOG(INFO) << "[UnsafeCache] 当前缓存大小: " << cache_.size();
        return cache_.size(); 
    }

private:
    std::unordered_map<std::string, std::string> cache_;
};

// 错误实现：全局缓存（带锁但设计不合理）
class LockedButFlawedCache {
public:
    // 获取缓存值（有并发问题）
    std::string get(const std::string& key) {
        LOG(INFO) << "[FlawedCache] 访问键: " << key;
        
        // 在锁外检查缓存
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                LOG(INFO) << "[FlawedCache] 缓存命中: " << key;
                return it->second;
            }
        }
        
        LOG(WARNING) << "[FlawedCache] 缓存未命中: " << key << " (开始计算新值)";
        usleep(5000); // 5ms延迟
        
        // 生成新值
        std::string value = "Value for " + key;
        
        // 加锁更新缓存
        {
            std::lock_guard<std::mutex> lock(mutex_);
            LOG(INFO) << "[FlawedCache] 添加新缓存: " << key << " = " << value;
            cache_[key] = value;
        }
        
        return value;
    }
    
    size_t size() const { 
        std::lock_guard<std::mutex> lock(mutex_);
        LOG(INFO) << "[FlawedCache] 当前缓存大小: " << cache_.size();
        return cache_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> cache_;
};

// 正确实现：线程安全的缓存
class ThreadSafeCache {
public:
    // 获取缓存值（线程安全）
    std::string get(const std::string& key) {
        LOG(INFO) << "[SafeCache] 访问键: " << key;
        
        // 首先尝试无锁读取
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                LOG(INFO) << "[SafeCache] 缓存命中: " << key;
                return it->second;
            }
        }
        
        LOG(WARNING) << "[SafeCache] 缓存未命中: " << key << " (开始计算新值)";
        usleep(5000); // 5ms延迟
        
        // 生成新值
        std::string value = "Value for " + key;
        
        // 加锁更新缓存
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            // 再次检查，避免其他线程已经更新
            auto it = cache_.find(key);
            if (it != cache_.end()) {
                LOG(INFO) << "[SafeCache] 二次检查命中: " << key << " (其他线程已添加)";
                return it->second;
            }
            
            LOG(INFO) << "[SafeCache] 添加新缓存: " << key << " = " << value;
            cache_[key] = value;
        }
        
        return value;
    }
    
    size_t size() const { 
        std::shared_lock<std::shared_mutex> lock(mutex_);
        LOG(INFO) << "[SafeCache] 当前缓存大小: " << cache_.size();
        return cache_.size();
    }

private:
    mutable std::shared_mutex mutex_; // 读写锁
    std::unordered_map<std::string, std::string> cache_;
};

// 正确实现：bthread本地缓存
class BthreadLocalCache {
public:
    BthreadLocalCache() {
        // 创建bthread本地存储键
        CHECK_EQ(0, bthread_key_create(&cache_key_, cache_destructor));
        LOG(INFO) << "创建BthreadLocalCache";
    }
    
    ~BthreadLocalCache() {
        bthread_key_delete(cache_key_);
        LOG(INFO) << "销毁BthreadLocalCache";
    }
    
    // 获取缓存值（bthread本地）
    std::string get(const std::string& key) {
        // 获取bthread本地缓存
        CacheMap* cache = get_or_create_cache();
        LOG(INFO) << "[BthreadCache] bthread=" << bthread_self() 
                  << " 访问键: " << key;
        
        // 在本地缓存中查找
        auto it = cache->find(key);
        if (it != cache->end()) {
            LOG(INFO) << "[BthreadCache] bthread=" << bthread_self()
                      << " 缓存命中: " << key;
            return it->second;
        }
        
        LOG(WARNING) << "[BthreadCache] bthread=" << bthread_self()
                     << " 缓存未命中: " << key << " (开始计算新值)";
        usleep(5000); // 5ms延迟
        
        // 生成新值并缓存
        std::string value = "Value for " + key;
        (*cache)[key] = value;
        
        LOG(INFO) << "[BthreadCache] bthread=" << bthread_self()
                  << " 添加新缓存: " << key << " = " << value;
        return value;
    }
    
    // 获取所有bthread的缓存大小总和（仅用于演示）
    size_t total_size() const {
        // 在实际应用中，通常不需要统计所有bthread的缓存
        // 这里仅用于演示
        std::lock_guard<std::mutex> lock(stats_mutex_);
        size_t total = 0;
        for (auto* cache : all_caches_) {
            total += cache->size();
        }
        LOG(INFO) << "[BthreadCache] 总缓存大小: " << total;
        return total;
    }

private:
    using CacheMap = std::unordered_map<std::string, std::string>;
    
    static void cache_destructor(void* data) {
        CacheMap* cache = static_cast<CacheMap*>(data);
        LOG(INFO) << "销毁Bthread缓存: bthread=" << bthread_self()
                  << ", 缓存大小=" << cache->size();
        
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            all_caches_.erase(cache);
            LOG(INFO) << "剩余Bthread缓存数量: " << all_caches_.size();
        }
        
        delete cache;
    }
    
    CacheMap* get_or_create_cache() {
        void* data = bthread_getspecific(cache_key_);
        if (!data) {
            data = new CacheMap();
            CHECK_EQ(0, bthread_setspecific(cache_key_, data));
            
            // 记录所有缓存（仅用于演示）
            std::lock_guard<std::mutex> lock(stats_mutex_);
            all_caches_.insert(static_cast<CacheMap*>(data));
            LOG(INFO) << "创建新的Bthread缓存: bthread=" << bthread_self()
                      << ", 当前缓存总数: " << all_caches_.size();
        }
        return static_cast<CacheMap*>(data);
    }

    bthread_key_t cache_key_;
    static std::mutex stats_mutex_; // 静态成员
    static std::set<CacheMap*> all_caches_; // 静态成员
};

// 初始化静态成员
std::mutex BthreadLocalCache::stats_mutex_;
std::set<BthreadLocalCache::CacheMap*> BthreadLocalCache::all_caches_;

// ====================== 服务实现 ======================

class EchoServiceImpl : public EchoService {
public:
    EchoServiceImpl() 
        : unsafe_cache_(),
          flawed_cache_(),
          safe_cache_(),
          bthread_cache_() {
        LOG(INFO) << "创建EchoServiceImpl";
    }
    
    virtual ~EchoServiceImpl() {
        LOG(INFO) << "销毁EchoServiceImpl";
    };
    
    virtual void Echo(google::protobuf::RpcController* cntl_base,
                      const EchoRequest* request,
                      EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        brpc::Controller* cntl = static_cast<brpc::Controller*>(cntl_base);

        LOG(INFO) << "===== 开始处理请求 =====";
        LOG(INFO) << "请求键: " << request->message();
        LOG(INFO) << "bthread=" << bthread_self() 
                  << ", pthread=" << pthread_self();

        // 获取 BLS 和 TLS 数据并递增
        int* bls_data = get_or_create_bls_data();
        int* tls_data = get_or_create_tls_data();

        (*bls_data)++;
        (*tls_data)++;

        LOG(INFO) << "BLS值: " << *bls_data << ", TLS值: " << *tls_data;

        // 获取当前线程和bthread ID
        bthread_t bthread_id = bthread_self();
        pthread_t pthread_id = pthread_self();
        bool is_bthread = (bthread_id != 0);
        
        // 获取请求键
        const std::string& key = request->message();
        
        // 使用各种缓存获取值
        LOG(INFO) << "使用不安全缓存获取值...";
        std::string unsafe_value = unsafe_cache_.get(key);
        
        LOG(INFO) << "使用带锁但有缺陷缓存获取值...";
        std::string flawed_value = flawed_cache_.get(key);
        
        LOG(INFO) << "使用线程安全缓存获取值...";
        std::string safe_value = safe_cache_.get(key);
        
        LOG(INFO) << "使用bthread本地缓存获取值...";
        std::string bthread_value = bthread_cache_.get(key);

        // 构造响应消息
        std::string msg = butil::string_printf(
            "==================== 请求详情 ====================\n"
            "请求键: %s\n"
            "当前上下文: %s\n"
            "Bthread ID: %lu\n"
            "Pthread ID: %lu\n"
            "\n"
            "=============== 本地存储计数器 ================\n"
            "BLS (bthread本地): %d\n"
            "TLS (pthread本地): %d\n"
            "\n"
            "===================== 缓存结果 ======================\n"
            "不安全全局缓存值: %s\n"
            "带锁但有缺陷缓存值: %s\n"
            "线程安全缓存值: %s\n"
            "Bthread本地缓存值: %s\n"
            "\n"
            "===================== 缓存统计 ======================\n"
            "不安全缓存大小: %zu\n"
            "带锁缓存大小: %zu\n"
            "安全缓存大小: %zu\n"
            "Bthread缓存总大小: %zu\n"
            "\n"
            "===================== 缓存设计建议 ======================\n"
            "1. 避免使用无锁全局缓存 → 存在并发问题\n"
            "2. 避免在锁外检查缓存 → 可能导致重复计算\n"
            "3. 推荐使用读写锁或双重检查锁定\n"
            "4. 对于只读数据，bthread本地缓存效率最高",
            key.c_str(),
            is_bthread ? "bthread" : "pthread",
            bthread_id,
            pthread_id,
            *bls_data,
            *tls_data,
            unsafe_value.c_str(),
            flawed_value.c_str(),
            safe_value.c_str(),
            bthread_value.c_str(),
            unsafe_cache_.size(),
            flawed_cache_.size(),
            safe_cache_.size(),
            bthread_cache_.total_size());
        
        response->set_message(msg);

        if (FLAGS_echo_attachment) {
            cntl->response_attachment().append(cntl->request_attachment());
        }
        
        LOG(INFO) << "===== 请求处理完成 =====";
    }

private:
    // 三种缓存实现示例
    UnsafeGlobalCache unsafe_cache_;
    LockedButFlawedCache flawed_cache_;
    ThreadSafeCache safe_cache_;
    BthreadLocalCache bthread_cache_;
};

}  // namespace example

int main(int argc, char* argv[]) {
    // 初始化日志系统
    logging::LoggingSettings settings;
    settings.logging_dest = logging::LOG_TO_FILE;
    settings.log_file = "brpc_cache_demo.log";
    settings.lock_log = logging::LOCK_LOG_FILE;
    settings.delete_old = logging::DELETE_OLD_LOG_FILE;
    logging::InitLogging(settings);
    
    // 设置日志级别 - 使用正确的常量
    logging::SetMinLogLevel(logging::BLOG_INFO);
    
    GFLAGS_NS::ParseCommandLineFlags(&argc, &argv, true);
    
    LOG(INFO) << "==================== 启动服务 ====================";
    LOG(INFO) << "端口: " << FLAGS_port;
    LOG(INFO) << "监听地址: " << FLAGS_listen_addr;
    
    // 初始化键
    init_keys();

    brpc::Server server;
    example::EchoServiceImpl echo_service_impl;

    if (server.AddService(&echo_service_impl, 
                          brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "添加服务失败";
        return -1;
    }

    butil::EndPoint point;
    if (!FLAGS_listen_addr.empty()) {
        if (butil::str2endpoint(FLAGS_listen_addr.c_str(), &point) < 0) {
            LOG(ERROR) << "无效的监听地址:" << FLAGS_listen_addr;
            return -1;
        }
    } else {
        point = butil::EndPoint(butil::IP_ANY, FLAGS_port);
    }
    
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    if (server.Start(point, &options) != 0) {
        LOG(ERROR) << "启动EchoServer失败";
        return -1;
    }

    LOG(INFO) << "服务已启动，端口: " << FLAGS_port;
    LOG(INFO) << "使用 'curl -d '{\"message\":\"key1\"}' http://localhost:" << FLAGS_port << "/example.EchoService/Echo' 进行测试";

    server.RunUntilAskedToQuit();
    
    LOG(INFO) << "服务停止中...";
    
    // 清理资源
    bthread_key_delete(bls_key);
    pthread_key_delete(tls_key);
    
    LOG(INFO) << "服务已停止";
    
    return 0;
}


