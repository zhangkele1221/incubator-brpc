// echo_server.cpp
#include <gflags/gflags.h>
#include <butil/logging.h>
#include <brpc/server.h>
#include <brpc/restful.h>

#include "echo.pb.h"
#include "validator_util.h"

DEFINE_bool(use_validation, true, "Enable parameter validation");
DEFINE_int32(port, 8000, "TCP Port of this server");
DEFINE_int32(idle_timeout_s, -1, "Connection will be closed if there is no read/write operations during the last `idle_timeout_s`");

namespace example {

class EchoServiceImpl : public EchoService {
public:
    EchoServiceImpl() {}
    virtual ~EchoServiceImpl() {}

    virtual void Echo(google::protobuf::RpcController* cntl_base,
                     const EchoRequest* request,
                     EchoResponse* response,
                     google::protobuf::Closure* done) {
        
        brpc::ClosureGuard done_guard(done);
        brpc::Controller* cntl = static_cast<brpc::Controller*>(cntl_base);
        
        // 参数自动校验
        if (FLAGS_use_validation) {
            auto validation_result = validator::ValidatorUtil::Validate(*request);
            if (!validation_result.is_valid) {
                cntl->SetFailed(brpc::EREQUEST, "Validation failed: %s", 
                              validation_result.msg.c_str());
                LOG(ERROR) << "Request validation failed: " << validation_result.msg 
                          << " from " << cntl->remote_side();
                return;
            }
            LOG(INFO) << "Request validation passed for " << cntl->remote_side();
        }
        
        // 处理业务逻辑
        ProcessEchoRequest(request, response, cntl);
        
        LOG(INFO) << "Processed EchoRequest: message=" << request->message()
                 << " repeat_count=" << request->repeat_count()
                 << " user_name=" << request->user_name();
    }

private:
    void ProcessEchoRequest(const EchoRequest* request, 
                          EchoResponse* response,
                          brpc::Controller* cntl) {
        
        // 构建重复的消息
        std::string processed_message;
        for (int i = 0; i < request->repeat_count(); ++i) {
            if (i > 0) processed_message += " ";
            processed_message += request->message();
        }
        
        response->set_message(processed_message);
        response->set_processed_count(request->repeat_count());
        response->set_status("SUCCESS");
        
        // 添加标签信息
        if (!request->tags().empty()) {
            std::string tags_str;
            for (const auto& tag : request->tags()) {
                if (!tags_str.empty()) tags_str += ", ";
                tags_str += tag;
            }
            LOG(INFO) << "Request tags: " << tags_str;
        }
    }
};

}  // namespace example

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    
    brpc::Server server;
    example::EchoServiceImpl echo_service_impl;
    
    if (server.AddService(&echo_service_impl, 
                         brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "Fail to add service";
        return -1;
    }
    
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    
    if (server.Start(FLAGS_port, &options) != 0) {
        LOG(ERROR) << "Fail to start EchoServer";
        return -1;
    }
    
    LOG(INFO) << "EchoServer is running on port " << FLAGS_port
             << " with validation: " << (FLAGS_use_validation ? "ENABLED" : "DISABLED");
    
    server.RunUntilAskedToQuit();
    return 0;
}