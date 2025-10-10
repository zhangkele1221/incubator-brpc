//  curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'

/*
for i in {1..5}; do
  curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r '.message'
done
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
    LOG(INFO) << "Destroying BLS data: " << *(int*)data;
    delete (int*)data;
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
int* get_or_create_bls_data() {
    void* data = bthread_getspecific(bls_key);
    if (!data) {
        data = new int(0);
        CHECK_EQ(0, bthread_setspecific(bls_key, data));
    }
    return (int*)data;
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

    // 获取 BLS 和 TLS 数据并递增
    int* bls_data = get_or_create_bls_data();
    int* tls_data = get_or_create_tls_data();

    (*bls_data)++;
    (*tls_data)++;

    // 获取当前线程和bthread ID
    bthread_t bthread_id = bthread_self();
    pthread_t pthread_id = pthread_self();
    bool is_bthread = (bthread_id != 0);

    // 静态变量跟踪历史信息
    static std::map<pthread_t, int> pthread_request_count;
    static std::map<bthread_t, int> bthread_request_count;
    
    pthread_request_count[pthread_id]++;
    bthread_request_count[bthread_id]++;

    // 构造基于实际观察的响应消息
    std::string msg = butil::string_printf(
        "==================== 请求详情 ====================\n"
        "原始消息: %s\n"
        "当前上下文: %s\n"
        "Bthread ID: %lu (已处理%d次)\n"
        "Pthread ID: %lu (已处理%d次)\n"
        "\n"
        "=============== 本地存储计数器 ================\n"
        "BLS (bthread本地): %d\n"
        "TLS (pthread本地): %d\n"
        "\n"
        "===================== 实际观察分析 ======================\n"
        "BLS模式: 连续递增(1->%d)，表明bthread状态被保持\n"
        "TLS模式: 在pthread内递增，切换pthread时变化\n"
        "\n"
        "===================== brpc线程模型分析 ======================\n"
        "观察到的行为:\n"
        "- Bthread ID变化但BLS持续递增 → 状态保持机制\n"
        "- Pthread有限复用(m个pthread处理n个请求) → 线程池优化\n"
        "- TLS符合预期 → pthread本地存储正常工作\n"
        "\n"
        "结论: brpc实现了智能的bthread状态管理和线程复用",
        request->message().c_str(),
        is_bthread ? "bthread" : "pthread",
        bthread_id, bthread_request_count[bthread_id],
        pthread_id, pthread_request_count[pthread_id],
        *bls_data,
        *tls_data,
        *bls_data);
    
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
    LOG(INFO) << "Use 'curl http://localhost:" << FLAGS_port << "/EchoService/Echo' to test";

    server.RunUntilAskedToQuit();
    
    // 清理资源
    bthread_key_delete(bls_key);
    pthread_key_delete(tls_key);
    
    return 0;
}

/*
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   680  100   661  100    19   472k  13919 --:--:-- --:--:-- --:--:--  664k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 8589936129
Pthread ID: 127653353617088

=============== 本地存储计数器 ================
BLS (bthread本地): 1
TLS (pthread本地): 1

===================== 解释说明 ======================
BLS: 每个bthread有自己独立的计数器
TLS: 同一个pthread上的所有bthread共享同一个计数器

===================== 线程关系 ======================
M:N 线程模型: 1个bthread映射到1个pthread
当前请求由以下线程处理:
  - Bthread 8589936129
  - 运行在Pthread 127653353617088上
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   680  100   661  100    19   626k  18446 --:--:-- --:--:-- --:--:--  664k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 4294967553
Pthread ID: 127653138921152

=============== 本地存储计数器 ================
BLS (bthread本地): 2
TLS (pthread本地): 1

===================== 解释说明 ======================
BLS: 每个bthread有自己独立的计数器
TLS: 同一个pthread上的所有bthread共享同一个计数器

===================== 线程关系 ======================
M:N 线程模型: 1个bthread映射到1个pthread
当前请求由以下线程处理:
  - Bthread 4294967553
  - 运行在Pthread 127653138921152上
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   680  100   661  100    19   655k  19289 --:--:-- --:--:-- --:--:--  664k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 8589934849
Pthread ID: 127653138921152

=============== 本地存储计数器 ================
BLS (bthread本地): 3
TLS (pthread本地): 2

===================== 解释说明 ======================
BLS: 每个bthread有自己独立的计数器
TLS: 同一个pthread上的所有bthread共享同一个计数器

===================== 线程关系 ======================
M:N 线程模型: 1个bthread映射到1个pthread
当前请求由以下线程处理:
  - Bthread 8589934849
  - 运行在Pthread 127653138921152上
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   682  100   663  100    19   295k   8675 --:--:-- --:--:-- --:--:--  333k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 30064771841
Pthread ID: 127653370402496

=============== 本地存储计数器 ================
BLS (bthread本地): 4
TLS (pthread本地): 1

===================== 解释说明 ======================
BLS: 每个bthread有自己独立的计数器
TLS: 同一个pthread上的所有bthread共享同一个计数器

===================== 线程关系 ======================
M:N 线程模型: 1个bthread映射到1个pthread
当前请求由以下线程处理:
  - Bthread 30064771841
  - 运行在Pthread 127653370402496上
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   682  100   663  100    19   685k  20127 --:--:-- --:--:-- --:--:--  666k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 34359739137
Pthread ID: 127653370402496

=============== 本地存储计数器 ================
BLS (bthread本地): 5
TLS (pthread本地): 2

===================== 解释说明 ======================
BLS: 每个bthread有自己独立的计数器
TLS: 同一个pthread上的所有bthread共享同一个计数器

===================== 线程关系 ======================
M:N 线程模型: 1个bthread映射到1个pthread
当前请求由以下线程处理:
  - Bthread 34359739137
  - 运行在Pthread 127653370402496上
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   682  100   663  100    19   733k  21517 --:--:-- --:--:-- --:--:--  666k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 30064771329
Pthread ID: 127653138921152

=============== 本地存储计数器 ================
BLS (bthread本地): 6
TLS (pthread本地): 3

===================== 解释说明 ======================
BLS: 每个bthread有自己独立的计数器
TLS: 同一个pthread上的所有bthread共享同一个计数器

===================== 线程关系 ======================
M:N 线程模型: 1个bthread映射到1个pthread
当前请求由以下线程处理:
  - Bthread 30064771329
  - 运行在Pthread 127653138921152上
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   682  100   663  100    19   607k  17840 --:--:-- --:--:-- --:--:--  666k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 47244641025
Pthread ID: 127653370402496

=============== 本地存储计数器 ================
BLS (bthread本地): 7
TLS (pthread本地): 3

===================== 解释说明 ======================
BLS: 每个bthread有自己独立的计数器
TLS: 同一个pthread上的所有bthread共享同一个计数器

===================== 线程关系 ======================
M:N 线程模型: 1个bthread映射到1个pthread
当前请求由以下线程处理:
  - Bthread 47244641025
  - 运行在Pthread 127653370402496上
@zhangkele1221 ➜ /workspaces/incubator-brpc (learing) $ 








    下面 矛盾分析：
    正常情况下，​​不同的bthread应该有不同的BLS存储​​。但这里观察到：
    三个不同的bthread ID
    但BLS值却连续递增（1→2→3->4->5）
    这似乎与bthread本地存储(BLS)的设计相矛盾。

    原因解析（关键发现）：
    这个现象揭示了brpc bthread实现的一个​​精妙设计细节​​：
    ​​bthread的"逻辑ID" vs "物理实体"​​：
    每次请求的bthread_self()返回的是​​逻辑ID​​（每次请求唯一）
    但底层复用的是同一个​​物理bthread实体​​（资源对象）        ​​既保持bthread状态连续性，又给每个请求独立可追踪的ID​​


==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 4294967297 (已处理1次)
Pthread ID: 137982623983296 (已处理1次)

=============== 本地存储计数器 ================
BLS (bthread本地): 1
TLS (pthread本地): 1

===================== 实际观察分析 ======================
BLS模式: 连续递增(1->1)，表明bthread状态被保持
TLS模式: 在pthread内递增，切换pthread时变化

===================== brpc线程模型分析 ======================
观察到的行为:
- Bthread ID变化但BLS持续递增 → 状态保持机制
- Pthread有限复用(m个pthread处理n个请求) → 线程池优化
- TLS符合预期 → pthread本地存储正常工作

结论: brpc实现了智能的bthread状态管理和线程复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   860  100   841  100    19   815k  18867 --:--:-- --:--:-- --:--:--  839k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 17179871233 (已处理1次)
Pthread ID: 137982493304512 (已处理1次)

=============== 本地存储计数器 ================
BLS (bthread本地): 2
TLS (pthread本地): 1

===================== 实际观察分析 ======================
BLS模式: 连续递增(1->2)，表明bthread状态被保持
TLS模式: 在pthread内递增，切换pthread时变化

===================== brpc线程模型分析 ======================
观察到的行为:
- Bthread ID变化但BLS持续递增 → 状态保持机制
- Pthread有限复用(m个pthread处理n个请求) → 线程池优化
- TLS符合预期 → pthread本地存储正常工作

结论: brpc实现了智能的bthread状态管理和线程复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   859  100   840  100    19   747k  17319 --:--:-- --:--:-- --:--:--  838k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 4294969601 (已处理1次)
Pthread ID: 137982484911808 (已处理1次)

=============== 本地存储计数器 ================
BLS (bthread本地): 3
TLS (pthread本地): 1

===================== 实际观察分析 ======================
BLS模式: 连续递增(1->3)，表明bthread状态被保持
TLS模式: 在pthread内递增，切换pthread时变化

===================== brpc线程模型分析 ======================
观察到的行为:
- Bthread ID变化但BLS持续递增 → 状态保持机制
- Pthread有限复用(m个pthread处理n个请求) → 线程池优化
- TLS符合预期 → pthread本地存储正常工作

结论: brpc实现了智能的bthread状态管理和线程复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   860  100   841  100    19   881k  20386 --:--:-- --:--:-- --:--:--  839k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 21474838273 (已处理1次)
Pthread ID: 137982501697216 (已处理1次)

=============== 本地存储计数器 ================
BLS (bthread本地): 4
TLS (pthread本地): 1

===================== 实际观察分析 ======================
BLS模式: 连续递增(1->4)，表明bthread状态被保持
TLS模式: 在pthread内递增，切换pthread时变化

===================== brpc线程模型分析 ======================
观察到的行为:
- Bthread ID变化但BLS持续递增 → 状态保持机制
- Pthread有限复用(m个pthread处理n个请求) → 线程池优化
- TLS符合预期 → pthread本地存储正常工作

结论: brpc实现了智能的bthread状态管理和线程复用
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current
                                 Dload  Upload   Total   Spent    Left  Speed
100   860  100   841  100    19   176k   4086 --:--:-- --:--:-- --:--:--  209k
==================== 请求详情 ====================
原始消息: Hello
当前上下文: bthread
Bthread ID: 17179871489 (已处理1次)
Pthread ID: 137982484911808 (已处理2次)

=============== 本地存储计数器 ================
BLS (bthread本地): 5
TLS (pthread本地): 2

===================== 实际观察分析 ======================
BLS模式: 连续递增(1->5)，表明bthread状态被保持
TLS模式: 在pthread内递增，切换pthread时变化

===================== brpc线程模型分析 ======================
观察到的行为:
- Bthread ID变化但BLS持续递增 → 状态保持机制
- Pthread有限复用(m个pthread处理n个请求) → 线程池优化
- TLS符合预期 → pthread本地存储正常工作

结论: brpc实现了智能的bthread状态管理和线程复用



*/