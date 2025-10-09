//  curl -d '{"message":"Hello"}' http://localhost:8000/example.EchoService/Echo | jq -r  '.message'

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

        // 构造响应消息
        std::string msg = butil::string_printf(
            "Original message: %s\n"
            "Current context: %s\n"
            "BLS count: %d (bthread local)\n"
            "TLS count: %d (pthread local)",
            request->message().c_str(),
            current_context(),
            *bls_data,
            *tls_data);
        
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