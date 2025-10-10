//  curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'

/*
// 串行的请求 
for i in {1..5}; do
  curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r '.message'
done
*/

/*
//改成并行的请求  这样服务就会创建多个   Created new physical bthread 多个物理bthread了

for i in {1..10000}; do
  curl -d '{"message":"Hello'$i'"}' http://localhost:8000/example.EchoService/Echo | jq -r '.message' > response_$i.txt &
done
wait
cat response_*.txt


*/

/*
深入解析 brpc 的 M:N 模型

1. ​​M:N 模型本质​​：
M个 ​​bthread (逻辑线程/协程)​​ 映射到 N个 ​​pthread (物理线程/内核线程)​​ 上执行。
一个 pthread ​​同时​​ 可能运行多个 bthread (通过用户态调度器切换)。
bthread 的切换 ​​不触发​​ pthread 的切换，因此 ​​不触发​​ 操作系统 TLS 的切换。

2.__thread的问题​​：

// 错误用法！
__thread MyCache per_thread_cache; // 每个 pthread 一个缓存

void my_bthread_function() {
    // 假设 bthread1 和 bthread2 被调度到同一个 pthread 上运行
    per_thread_cache.get(...); // bthread1 和 bthread2 会共享同一个缓存实例！
}

​​后果​​：同一个 pthread 上运行的所有 bthread ​​共享同一个 TLS 变量​​。
导致 ​​数据污染​​：bthread1 的操作会破坏 bthread2 的缓存数据。
引发 ​​并发冲突​​：多个 bthread 同时读写同一个缓存对象，需要额外加锁，完全失去本地缓存的意义。

​​3. BLS 的正确姿势​​：

        #include <brpc/thread_local_data.h>

        正确！每个 bthread 独立缓存
        void init_my_cache(void* data) { / 初始化缓存/ }
        void destroy_my_cache(void* data) { /清理缓存/ }

        // 创建 BLS 键 (通常在全局初始化)
        static bthread_key_t cache_key;
        pthread_once_t key_once = PTHREAD_ONCE_INIT;
        void create_cache_key() {
            bthread_key_create(&cache_key, destroy_my_cache);
        }

        MyCache* get_bthread_cache() {
            pthread_once(&key_once, create_cache_key);

            // 获取当前 bthread 的缓存
            MyCache* cache = static_cast<MyCache*>(bthread_getspecific(cache_key));
            if (!cache) {
                cache = new MyCache(); // 创建新缓存
                bthread_setspecific(cache_key, cache);
            }
            return cache;
        }

        void my_bthread_function() {
            MyCache* cache = get_bthread_cache(); // 每个 bthread 有自己的 cache
            cache->get(...); // 安全，无竞争
        }


总结
​​绝对不要​​在 bthread 函数中使用 __thread或 thread_local存储与​​单个请求/任务​​相关的状态或缓存。
​​必须使用​​ brpc 提供的 ​​BLS 机制​​ (bthread_key_create, bthread_getspecific, bthread_setspecific) 来实现 bthread 局部存储。
​​资源管理​​：
在 bthread_key_create时提供 destructor函数，确保 bthread 结束时自动释放资源（如删除缓存对象）。
避免在 BLS 中存储过多或过大的数据，防止内存膨胀。
​​性能考虑​​：
BLS 访问比 TLS 稍慢（需要一次哈希查找），但仍是高效的用户态操作。
对于超高性能场景，可考虑将 BLS 指针存储在 bthread 的 ​​上下文 (Context)​​ 中（如果 brpc 版本支持自定义上下文）。
​​替代方案​​：
对于​​只读​​或​​线程安全​​的全局数据，可直接使用全局缓存（需加锁或使用并发数据结构）。
对于​​与物理线程绑定的资源​​（如线程池、硬件加速器上下文），使用 __thread是合适的。
​​核心结论​​：在 brpc 的 bthread 环境中，__thread绑定的是物理线程 (pthread)，而 ​​bthread 需要的是逻辑线程隔离的存储 (BLS)​​。混淆二者会导致严重的数据竞争和错误，必须严格区分使用场景。


*/
#include <gflags/gflags.h>
#include <butil/logging.h>
#include <brpc/server.h>
#include <bthread/bthread.h>
#include <pthread.h>
#include <atomic>
#include <map>
#include <mutex>
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

// 定义BLS数据结构
struct BLSData {
    int counter;                // 计数器
    uint64_t physical_id;        // 物理bthread的唯一标识
    uint64_t create_time_us;     // 创建时间（微秒）
    std::vector<uint64_t> logical_ids; // 处理过的逻辑bthread ID
};

// 全局变量用于分配物理ID和跟踪物理bthread
static std::atomic<uint64_t> next_physical_id(1);
static std::mutex bls_map_mutex;
static std::map<uint64_t, BLSData*> physical_bls_map; // 物理ID到BLS数据的映射

// BLS 和 TLS 的析构函数
void bls_destructor(void* data) {
    BLSData* bls_data = static_cast<BLSData*>(data);
    LOG(INFO) << "Destroying BLS data: Physical ID=" << bls_data->physical_id
              << ", Final Count=" << bls_data->counter
              << ", Handled " << bls_data->logical_ids.size() << " requests";
    {
        std::lock_guard<std::mutex> lock(bls_map_mutex);
        physical_bls_map.erase(bls_data->physical_id);
    }
    delete bls_data;
}

void tls_destructor(void* data) {
    LOG(INFO) << "Destroying TLS data: " << *(int*)data;
    delete (int*)data;
}

// 初始化键
void init_keys() {
    CHECK_EQ(0, bthread_key_create(&bls_key, bls_destructor));
    CHECK_EQ(0, pthread_key_create(&tls_key, tls_destructor));
}

// 获取当前上下文的字符串表示
const char* current_context() {
    return bthread_self() != 0 ? "bthread" : "pthread";
}

// 获取或创建当前 BLS 的数据
BLSData* get_or_create_bls_data() {
    void* data = bthread_getspecific(bls_key);
    if (!data) {
        // 创建新的BLS数据
        BLSData* new_data = new BLSData();
        new_data->counter = 0;
        new_data->physical_id = next_physical_id.fetch_add(1);
        new_data->create_time_us = butil::gettimeofday_us();
        
        {
            std::lock_guard<std::mutex> lock(bls_map_mutex);
            physical_bls_map[new_data->physical_id] = new_data;
        }
        
        CHECK_EQ(0, bthread_setspecific(bls_key, new_data));
        data = new_data;
        
        LOG(INFO) << "Created new physical bthread: ID=" << new_data->physical_id;
    }
    return static_cast<BLSData*>(data);
}

// 获取或创建当前 TLS 的数据
int* get_or_create_tls_data() {
    void* data = pthread_getspecific(tls_key);
    if (!data) {
        data = new int(0);
        CHECK_EQ(0, pthread_setspecific(tls_key, data));
    }
    return (int*)data;
}

// 辅助函数：格式化时间
std::string format_time(uint64_t usec) {
    time_t sec = usec / 1000000;
    struct tm tm_time;
    localtime_r(&sec, &tm_time);
    
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_time);
    return std::string(buf);
}

namespace example {

class EchoServiceImpl : public EchoService {
public:
    EchoServiceImpl() {};
    virtual ~EchoServiceImpl() {};
    
    virtual void Echo(google::protobuf::RpcController* cntl_base,
                      const EchoRequest* request,
                      EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        brpc::Controller* cntl = static_cast<brpc::Controller*>(cntl_base);

        // 获取 BLS 和 TLS 数据
        BLSData* bls_data = get_or_create_bls_data();
        int* tls_data = get_or_create_tls_data();

        // 递增计数器
        bls_data->counter++;
        (*tls_data)++;

        // 获取当前线程和bthread ID
        bthread_t bthread_id = bthread_self();
        pthread_t pthread_id = pthread_self();
        bool is_bthread = (bthread_id != 0);

        // 记录逻辑bthread ID
        bls_data->logical_ids.push_back(bthread_id);

        // 静态变量跟踪历史信息
        static std::mutex history_mutex;
        static std::map<pthread_t, int> pthread_request_count;
        static std::map<bthread_t, int> bthread_request_count;
        
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            pthread_request_count[pthread_id]++;
            bthread_request_count[bthread_id]++;
        }

        // 计算物理bthread的复用次数
        int reuse_count = bls_data->counter - 1;

        // 构造响应消息
        std::string msg = butil::string_printf(
            "==================== 请求详情 ====================\n"
            "原始消息: %s\n"
            "当前上下文: %s\n"
            "逻辑Bthread ID: %lu (已处理%d次)\n"
            "物理Bthread ID: %lu (已复用%d次)\n"
            "Pthread ID: %lu (已处理%d次)\n"
            "\n"
            "=============== 本地存储计数器 ================\n"
            "BLS (物理bthread本地): %d (创建时间: %s)\n"
            "TLS (pthread本地): %d\n"
            "\n"
            "===================== BLS分析 ======================\n"
            "物理bthread状态: %s\n"
            "已处理逻辑bthread数量: %zu\n"
            "BLS值变化模式: %s\n"
            "\n"
            "===================== brpc线程模型分析 ======================\n"
            "关键观察:\n"
            "- 物理bthread ID保持不变 → 底层资源复用\n"
            "- BLS计数器持续递增 → 状态保持机制\n"
            "- 多个逻辑bthread共享同一物理bthread → 高效复用",
            request->message().c_str(),
            is_bthread ? "bthread" : "pthread",
            bthread_id, bthread_request_count[bthread_id],
            bls_data->physical_id, reuse_count,
            pthread_id, pthread_request_count[pthread_id],
            bls_data->counter, format_time(bls_data->create_time_us).c_str(),
            *tls_data,
            reuse_count > 0 ? "复用中" : "新创建",
            bls_data->logical_ids.size(),
            reuse_count > 0 ? "持续递增(状态保持)" : "初始值(新bthread)");
        
        response->set_message(msg);

        if (FLAGS_echo_attachment) {
            cntl->response_attachment().append(cntl->request_attachment());
        }
    }
};

}  // namespace example

int main(int argc, char* argv[]) {
    GFLAGS_NS::ParseCommandLineFlags(&argc, &argv, true);
    
    // 初始化键
    init_keys();

    brpc::Server server;
    example::EchoServiceImpl echo_service_impl;

    if (server.AddService(&echo_service_impl, 
                          brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Fail to add service";
        return -1;
    }

    butil::EndPoint point;
    if (!FLAGS_listen_addr.empty()) {
        if (butil::str2endpoint(FLAGS_listen_addr.c_str(), &point) < 0) {
            LOG(ERROR) << "Invalid listen address:" << FLAGS_listen_addr;
            return -1;
        }
    } else {
        point = butil::EndPoint(butil::IP_ANY, FLAGS_port);
    }
    
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    if (server.Start(point, &options) != 0) {
        LOG(ERROR) << "Fail to start EchoServer";
        return -1;
    }

    LOG(INFO) << "Server started on port " << FLAGS_port;
    LOG(INFO) << "Use 'curl http://localhost:" << FLAGS_port << "/example.EchoService/Echo' to test";

    server.RunUntilAskedToQuit();
    
    // 清理资源
    bthread_key_delete(bls_key);
    pthread_key_delete(tls_key);
    
    return 0;
}



/*

关键特性展示：
​​物理bthread vs 逻辑bthread​​：
物理ID保持不变，表示底层资源被复用
逻辑ID每次变化，表示每次请求的独立标识
​​BLS状态保持​​：
当物理bthread被复用时，BLS计数器持续递增
新物理bthread创建时，BLS计数器从1开始
​​资源复用可视化​​：
显示物理bthread已处理的逻辑bthread数量
显示物理bthread的创建时间和复用次数
​​线程模型分析​​：
在响应中直接解释观察到的现象

对 context_demo.cpp 现象的一个补充



  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   833  100   814  100    19   239k   5728 --:--:-- --:--:-- --:--:--  271k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 8589935105 (已处理1次)
物理Bthread ID: 1 (已复用0次)
Pthread ID: 133892140934848 (已处理1次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 1 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 1

===================== BLS分析 ======================
物理bthread状态: 新创建
已处理逻辑bthread数量: 1
BLS值变化模式: 初始值(新bthread)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   839  100   820  100    19   500k  11867 --:--:-- --:--:-- --:--:--  819k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 21474836993 (已处理1次)
物理Bthread ID: 1 (已复用1次)
Pthread ID: 133892140934848 (已处理2次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 2 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 2

===================== BLS分析 ======================
物理bthread状态: 复用中
已处理逻辑bthread数量: 2
BLS值变化模式: 持续递增(状态保持)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   838  100   819  100    19   179k   4267 --:--:-- --:--:-- --:--:--  204k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 4294968321 (已处理1次)
物理Bthread ID: 1 (已复用2次)
Pthread ID: 133892132542144 (已处理1次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 3 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 1

===================== BLS分析 ======================
物理bthread状态: 复用中
已处理逻辑bthread数量: 3
BLS值变化模式: 持续递增(状态保持)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   839  100   820  100    19   118k   2815 --:--:-- --:--:-- --:--:--  136k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 12884903169 (已处理1次)
物理Bthread ID: 1 (已复用3次)
Pthread ID: 133892182898368 (已处理1次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 4 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 1

===================== BLS分析 ======================
物理bthread状态: 复用中
已处理逻辑bthread数量: 4
BLS值变化模式: 持续递增(状态保持)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   838  100   819  100    19   687k  16337 --:--:-- --:--:-- --:--:--  818k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 8589935617 (已处理1次)
物理Bthread ID: 1 (已复用4次)
Pthread ID: 133892132542144 (已处理2次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 5 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 2

===================== BLS分析 ======================
物理bthread状态: 复用中
已处理逻辑bthread数量: 5
BLS值变化模式: 持续递增(状态保持)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   839  100   820  100    19   262k   6231 --:--:-- --:--:-- --:--:--  273k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 12884902657 (已处理1次)
物理Bthread ID: 1 (已复用5次)
Pthread ID: 133892174505664 (已处理1次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 6 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 1

===================== BLS分析 ======================
物理bthread状态: 复用中
已处理逻辑bthread数量: 6
BLS值变化模式: 持续递增(状态保持)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   839  100   820  100    19   730k  17335 --:--:-- --:--:-- --:--:--  819k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 30064772353 (已处理1次)
物理Bthread ID: 1 (已复用6次)
Pthread ID: 133892182898368 (已处理2次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 7 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 2

===================== BLS分析 ======================
物理bthread状态: 复用中
已处理逻辑bthread数量: 7
BLS值变化模式: 持续递增(状态保持)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   839  100   820  100    19   303k   7207 --:--:-- --:--:-- --:--:--  409k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 42949674241 (已处理1次)
物理Bthread ID: 1 (已复用7次)
Pthread ID: 133892182898368 (已处理3次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 8 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 3

===================== BLS分析 ======================
物理bthread状态: 复用中
已处理逻辑bthread数量: 8
BLS值变化模式: 持续递增(状态保持)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   839  100   820  100    19   253k   6008 --:--:-- --:--:-- --:--:--  273k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 17179869953 (已处理1次)
物理Bthread ID: 1 (已复用8次)
Pthread ID: 133892174505664 (已处理2次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 9 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 2

===================== BLS分析 ======================
物理bthread状态: 复用中
已处理逻辑bthread数量: 9
BLS值变化模式: 持续递增(状态保持)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   841  100   822  100    19   336k   7963 --:--:-- --:--:-- --:--:--  410k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
逻辑Bthread ID: 30064771841 (已处理1次)
物理Bthread ID: 1 (已复用9次)
Pthread ID: 133892174505664 (已处理3次)

=============== 本地存储计数器 ================
BLS (物理bthread本地): 10 (创建时间: 2025-10-10 03:53:07)
TLS (pthread本地): 3

===================== BLS分析 ======================
物理bthread状态: 复用中
已处理逻辑bthread数量: 10
BLS值变化模式: 持续递增(状态保持)

===================== brpc线程模型分析 ======================
关键观察:
- 物理bthread ID保持不变 → 底层资源复用
- BLS计数器持续递增 → 状态保持机制
- 多个逻辑bthread共享同一物理bthread → 高效复用



*/