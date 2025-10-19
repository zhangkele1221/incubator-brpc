# 手动编译 proto 文件
protoc -I=proto --cpp_out=. proto/validator.proto
protoc -I=proto --cpp_out=. proto/echo.proto

# 手动编译程序
g++ -std=c++11 -I. -I/path/to/brpc/include echo_server.cpp validator.pb.cc echo.pb.cc -o echo_server -lbrpc -lprotobuf
g++ -std=c++11 -I. -I/path/to/brpc/include echo_client.cpp validator.pb.cc echo.pb.cc -o echo_client -lbrpc -lprotobuf


/path/to/brpc/include替换为实际的 brpc 头文件路径。
