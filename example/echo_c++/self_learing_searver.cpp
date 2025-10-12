// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

// 一个简单的 Echo 服务实现
// 接收 EchoRequest 消息并返回 EchoResponse 消息

// 参考文档：
// https://blog.csdn.net/KIDGIN7439/article/details/112243802
// https://segmentfault.com/a/1190000042407045

#include <gflags/gflags.h>       // Google Flags 库，用于命令行参数解析
#include <butil/logging.h>       // BRPC 日志库
#include <brpc/server.h>         // BRPC 服务器核心头文件
#include "echo.pb.h"             // 由 Protobuf 生成的 EchoService 接口

// 定义命令行参数
DEFINE_bool(echo_attachment, true, "是否回显附件数据");
DEFINE_int32(port, 8000, "服务器监听的 TCP 端口");
DEFINE_string(listen_addr, "", "服务器监听地址，可以是 IPV4/IPV6/UDS\n"
              "如果设置了此参数，port 参数将被忽略");
DEFINE_int32(idle_timeout_s, -1, "连接空闲超时时间（秒）\n"
             "如果在此时间内没有读写操作，连接将被关闭");
DEFINE_int32(logoff_ms, 2000, "服务器 LOGOFF 状态的最大持续时间（毫秒）\n"
             "（服务器停止前等待客户端关闭连接的时间）");

// 实现 EchoService 接口
namespace example {

class EchoServiceImpl : public EchoService {
public:
    EchoServiceImpl() {};
    virtual ~EchoServiceImpl() {};
    
    // 实现 Echo 方法
    virtual void Echo(google::protobuf::RpcController* cntl_base,
                      const EchoRequest* request,
                      EchoResponse* response,
                      google::protobuf::Closure* done) {
        // 使用 RAII 方式管理 Closure ，确保在函数退出时调用 done->Run()
        brpc::ClosureGuard done_guard(done);

        // 将通用的 RpcController 转换为 BRPC 的 Controller
        brpc::Controller* cntl = static_cast<brpc::Controller*>(cntl_base);

        // 记录请求日志（生产环境中可移除或降低日志级别）
        LOG(INFO) << "收到请求 [log_id=" << cntl->log_id() 
                  << "] 来自 " << cntl->remote_side() 
                  << " 到 " << cntl->local_side()
                  << ": " << request->message()
                  << " (附件=" << cntl->request_attachment() << ")";

        // 设置响应消息：将请求的消息内容原样返回
        response->set_message(request->message());

        // 如果需要，可以设置响应压缩（注意性能影响）
        // cntl->set_response_compress_type(brpc::COMPRESS_TYPE_GZIP);

        // 如果启用了附件回显，将请求附件附加到响应附件
        if (FLAGS_echo_attachment) {
            cntl->response_attachment().append(cntl->request_attachment());
        }
    }
};

}  // namespace example

int main(int argc, char* argv[]) {
    // 解析命令行参数
    GFLAGS_NS::ParseCommandLineFlags(&argc, &argv, true);

    // 创建 BRPC 服务器实例
    brpc::Server server;

    // 创建服务实现实例（在栈上）
    example::EchoServiceImpl echo_service_impl;

    // 将服务添加到服务器
    // 参数说明：
    //   &echo_service_impl - 服务实例指针
    //   brpc::SERVER_DOESNT_OWN_SERVICE - 服务器不拥有服务实例的所有权
    //
    // 生命周期控制:
    //   使用 brpc::SERVER_DOESNT_OWN_SERVICE 表示：
    //     - 服务器不会负责释放 echo_service_impl 实例
    //     - 开发者需要管理该对象的生命周期
    //     - 本例中服务实例在栈上创建，main函数结束时自动销毁
    //   如果服务在堆上创建(new)，需要手动delete
    if (server.AddService(&echo_service_impl, 
                          brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
        LOG(ERROR) << "添加服务失败";
        return -1;
    }

    // 设置监听端点
    butil::EndPoint point;
    if (!FLAGS_listen_addr.empty()) {
        // 解析监听地址
        if (butil::str2endpoint(FLAGS_listen_addr.c_str(), &point) < 0) {
            LOG(ERROR) << "无效的监听地址: " << FLAGS_listen_addr;
            return -1;
        }
    } else {
        // 使用默认IP和端口
        point = butil::EndPoint(butil::IP_ANY, FLAGS_port);
    }
    
    // 配置服务器选项
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s; // 设置空闲超时

    // 启动服务器
    // 内部流程：
    //   1. 调用 InitializeOnce() 初始化全局资源
    //   2. 创建监听套接字
    //   3. 初始化 bthread 环境
    //   4. 开始接受连接
    if (server.Start(point, &options) != 0) {
        LOG(ERROR) << "启动 EchoServer 失败";
        return -1;
    }

    // 等待直到收到停止信号（如 Ctrl-C）
    // 服务器将在此处阻塞，直到收到停止信号
    server.RunUntilAskedToQuit();
    
    return 0;
}