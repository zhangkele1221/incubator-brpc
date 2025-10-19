// echo_client.cpp
#include <gflags/gflags.h>
#include <butil/logging.h>

#include <brpc/channel.h>

#include "echo.pb.h"

DEFINE_string(server, "0.0.0.0:8000", "IP Address of server");
DEFINE_string(load_balancer, "", "The algorithm for load balancing");
DEFINE_int32(timeout_ms, 100, "RPC timeout in milliseconds");
DEFINE_int32(max_retry, 3, "Max retries(not including the first RPC)");

void TestValidRequest(brpc::Channel& channel) {
    example::EchoService_Stub stub(&channel);
    example::EchoRequest request;
    example::EchoResponse response;
    brpc::Controller cntl;
    
    // 构造合法的请求
    request.set_message("Hello, World!");
    request.set_repeat_count(3);
    request.set_user_name("John Doe");
    request.set_user_age(30);
    request.set_email("zhangkele1221@163.com");
    request.add_tags("test");
    request.add_tags("demo");
    request.set_priority(2);
    
    stub.Echo(&cntl, &request, &response, NULL);
    
    if (!cntl.Failed()) {
        LOG(INFO) << "Valid request - Received: " << response.message()
                 << " Status: " << response.status();
    } else {
        LOG(ERROR) << "Valid request failed: " << cntl.ErrorText();
    }
}

void TestInvalidRequest(brpc::Channel& channel) {
    example::EchoService_Stub stub(&channel);
    example::EchoRequest request;
    example::EchoResponse response;
    brpc::Controller cntl;
    
    // 构造非法的请求（违反校验规则）
    request.set_message("");  // 空消息，违反not_empty规则
    request.set_repeat_count(15);  // 超过最大值10
    request.set_user_name("ThisIsAVeryLongUserNameThatExceedsTheMaximumLengthLimit");
    request.set_user_age(200);  // 年龄超过150
    request.set_email("invalid-email");  // 无效邮箱格式
    request.set_priority(10);  // 不在允许范围内
    
    stub.Echo(&cntl, &request, &response, NULL);
    
    if (cntl.Failed()) {
        LOG(INFO) << "Invalid request correctly rejected: " << cntl.ErrorText();
    } else {
        LOG(ERROR) << "Invalid request was incorrectly accepted";
    }
}

void TestEdgeCases(brpc::Channel& channel) {
    example::EchoService_Stub stub(&channel);
    
    // 测试边界情况
    struct TestCase {
        std::string name;
        example::EchoRequest request;
        bool should_succeed;
    };
    
    std::vector<TestCase> test_cases = {
        {"Empty tags", []() {
            example::EchoRequest req;
            req.set_message("test");
            req.set_repeat_count(1);
            req.set_user_name("test");
            // tags为空，违反not_empty规则
            return req;
        }(), false},
        
        {"Valid email optional", []() {
            example::EchoRequest req;
            req.set_message("test");
            req.set_repeat_count(1);
            req.set_user_name("test");
            req.add_tags("test");
            req.set_priority(1);
            // 邮箱为空但可选，应该通过
            return req;
        }(), true},
        
        {"Max boundary", []() {
            example::EchoRequest req;
            req.set_message("A");  // 最小长度1
            req.set_repeat_count(10);  // 最大值
            req.set_user_name("MaxName");  // 在长度限制内
            req.set_user_age(150);  // 最大值
            req.set_email("test@example.com");
            req.add_tags("tag1");
            req.add_tags("tag2");
            req.set_priority(5);  // 最大值
            return req;
        }(), true}
    };
    
    for (const auto& test_case : test_cases) {
        example::EchoResponse response;
        brpc::Controller cntl;
        
        stub.Echo(&cntl, &test_case.request, &response, NULL);
        
        bool success = !cntl.Failed();
        if (success == test_case.should_succeed) {
            LOG(INFO) << "Test '" << test_case.name << "' PASSED";
        } else {
            LOG(ERROR) << "Test '" << test_case.name << "' FAILED: " 
                      << (success ? "Unexpected success" : cntl.ErrorText());
        }
    }
}

int main(int argc, char* argv[]) {
    google::ParseCommandLineFlags(&argc, &argv, true);
    
    brpc::Channel channel;
    brpc::ChannelOptions options;
    options.timeout_ms = FLAGS_timeout_ms;
    options.max_retry = FLAGS_max_retry;
    
    if (channel.Init(FLAGS_server.c_str(), FLAGS_load_balancer.c_str(), &options) != 0) {
        LOG(ERROR) << "Fail to initialize channel";
        return -1;
    }
    
    LOG(INFO) << "Testing parameter validation with brpc...";
    
    // 测试合法请求
    //LOG(INFO) << "\n=== Testing Valid Request ===";
    //TestValidRequest(channel);
    
    // 测试非法请求
    LOG(INFO) << "\n=== Testing Invalid Request ===";
    TestInvalidRequest(channel);
    
    // 测试边界情况
    //LOG(INFO) << "\n=== Testing Edge Cases ===";
    //TestEdgeCases(channel);
    
    LOG(INFO) << "Parameter validation test completed";
    return 0;
}