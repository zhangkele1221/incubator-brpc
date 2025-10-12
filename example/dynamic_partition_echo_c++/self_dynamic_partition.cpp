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

// 一个更高级的 Echo 服务实现
// 支持多个服务实例、延迟注入、异常模拟和性能统计

#include <vector>
#include <gflags/gflags.h>
#include <butil/time.h>
#include <butil/logging.h>
#include <butil/string_printf.h>
#include <butil/string_splitter.h>
#include <butil/rand_util.h>
#include <brpc/server.h>
#include "echo.pb.h"

// 定义命令行参数
DEFINE_bool(echo_attachment, true, "是否回显附件数据");
DEFINE_int32(port, 8004, "服务器监听的 TCP 端口");
DEFINE_int32(idle_timeout_s, -1, "连接空闲超时时间（秒）");
DEFINE_int32(logoff_ms, 2000, "服务器 LOGOFF 状态的最大持续时间（毫秒）");
DEFINE_int32(max_concurrency, 0, "最大并发请求处理数");
DEFINE_int32(server_num, 1, "服务器实例数量");
DEFINE_string(sleep_us, "", "响应前睡眠时间（微秒），逗号分隔多个值");
DEFINE_bool(spin, false, "使用自旋等待而不是睡眠");
DEFINE_double(exception_ratio, 0.1, "异常延迟比例");
DEFINE_double(min_ratio, 0.2, "最小睡眠时间比例");
DEFINE_double(max_ratio, 10, "最大睡眠时间比例");

// 实现 EchoService
class EchoServiceImpl : public example::EchoService {
public:
    EchoServiceImpl() : _index(0) {}
    virtual ~EchoServiceImpl() {};
    
    // 设置服务实例索引和睡眠时间
    void set_index(size_t index, int64_t sleep_us) { 
        _index = index; 
        _sleep_us = sleep_us;
    }
    
    // 实现 Echo 方法
    virtual void Echo(google::protobuf::RpcController* cntl_base,
                      const example::EchoRequest* request,
                      example::EchoResponse* response,
                      google::protobuf::Closure* done) {
        brpc::ClosureGuard done_guard(done);
        brpc::Controller* cntl = static_cast<brpc::Controller*>(cntl_base);
        
        // 延迟注入逻辑
        if (_sleep_us > 0) {
            double delay = _sleep_us;
            
            // 异常延迟模拟：按比例产生更短或更长的延迟
            const double a = FLAGS_exception_ratio * 0.5;
            if (a >= 0.0001) {
                double x = butil::RandDouble();
                if (x < a) {
                    // 产生较短的延迟
                    const double min_sleep_us = FLAGS_min_ratio * _sleep_us;
                    delay = min_sleep_us + (_sleep_us - min_sleep_us) * x / a;
                } else if (x + a > 1) {
                    // 产生较长的延迟
                    const double max_sleep_us = FLAGS_max_ratio * _sleep_us;
                    delay = _sleep_us + (max_sleep_us - _sleep_us) * (x + a - 1) / a;
                }
            }
            
            // 执行延迟（自旋或睡眠）
            if (FLAGS_spin) {
                int64_t end_time = butil::gettimeofday_us() + (int64_t)delay;
                while (butil::gettimeofday_us() < end_time) {} // 自旋等待
            } else {
                bthread_usleep((int64_t)delay); // 睡眠等待
            }
        }

        // 正常处理逻辑
        response->set_message(request->message());
        if (FLAGS_echo_attachment) {
            cntl->response_attachment().append(cntl->request_attachment());
        }
        
        // 更新请求计数
        _nreq << 1;
    }

    // 获取请求总数
    size_t num_requests() const { return _nreq.get_value(); }

private:
    size_t _index;       // 服务实例索引
    int64_t _sleep_us;   // 基础延迟时间（微秒）
    bvar::Adder<size_t> _nreq; // 请求计数器（原子操作）
};

int main(int argc, char* argv[]) {
    // 解析命令行参数
    GFLAGS_NS::ParseCommandLineFlags(&argc, &argv, true);

    // 验证参数
    if (FLAGS_server_num <= 0) {
        LOG(ERROR) << "server_num 必须为正数";
        return -1;
    }

    // 创建多个服务器实例
    brpc::Server* servers = new brpc::Server[FLAGS_server_num];
    
    // 配置服务器选项
    brpc::ServerOptions options;
    options.idle_timeout_sec = FLAGS_idle_timeout_s;
    options.max_concurrency = FLAGS_max_concurrency; // 最大并发控制

    // 解析延迟配置（支持逗号分隔的多个值）
    std::vector<int64_t> sleep_list;
    butil::StringSplitter sp(FLAGS_sleep_us.c_str(), ',');
    for (; sp; ++sp) {
        sleep_list.push_back(strtoll(sp.field(), NULL, 10));
    }
    if (sleep_list.empty()) {
        sleep_list.push_back(0); // 默认无延迟
    }

    // 创建多个服务实例
    EchoServiceImpl* echo_service_impls = new EchoServiceImpl[FLAGS_server_num];
    
    // 配置并启动多个服务实例
    for (int i = 0; i < FLAGS_server_num; ++i) {
        // 获取该实例的延迟时间
        int64_t sleep_us = sleep_list[(size_t)i < sleep_list.size() ? i : (sleep_list.size() - 1)];
        echo_service_impls[i].set_index(i, sleep_us);
        
        // 设置服务版本（用于动态分区）
        servers[i].set_version(butil::string_printf(
                    "example/dynamic_partition_echo_c++[%d]", i));
        
        // 添加服务
        if (servers[i].AddService(&echo_service_impls[i], 
                                  brpc::SERVER_DOESNT_OWN_SERVICE) != 0) {
            LOG(ERROR) << "添加服务失败";
            return -1;
        }
        
        // 启动服务器（每个实例监听不同端口）
        int port = FLAGS_port + i;
        if (servers[i].Start(port, &options) != 0) {
            LOG(ERROR) << "启动 EchoServer 失败";
            return -1;
        }
    }

    // 主循环：定期打印请求统计
    std::vector<size_t> last_num_requests(FLAGS_server_num);
    while (!brpc::IsAskedToQuit()) {
        sleep(1);
        
        size_t cur_total = 0;
        for (int i = 0; i < FLAGS_server_num; ++i) {
            const size_t current_num_requests =
                    echo_service_impls[i].num_requests();
            size_t diff = current_num_requests - last_num_requests[i];
            cur_total += diff;
            last_num_requests[i] = current_num_requests;
            LOG(INFO) << "S[" << i << "]=" << diff << ' ' << noflush;
        }
        LOG(INFO) << "[total=" << cur_total << ']';
    }

    // 优雅停止服务器
    for (int i = 0; i < FLAGS_server_num; ++i) {
        servers[i].Stop(FLAGS_logoff_ms); // 停止接受新请求
    }
    for (int i = 0; i < FLAGS_server_num; ++i) {
        servers[i].Join(); // 等待所有请求处理完成
    }
    
    // 清理资源
    delete[] servers;
    delete[] echo_service_impls;
    return 0;
}