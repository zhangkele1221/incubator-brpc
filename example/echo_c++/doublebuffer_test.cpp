#include<iostream>
#include<limits.h>

//  动态库编译方法
//g++ doublebuffer_test.cpp  -I/workspace/incubator-brpc/output/include   -L/workspace/incubator-brpc/output/lib -lbrpc  -o your_program 
// export LD_LIBRARY_PATH=/path/to/brpc/output/lib:$LD_LIBRARY_PATH  环境变量制定动态库的路径
//#include "../../src/butil/DoublyBufferedData.h"
#include <butil/containers/doubly_buffered_data.h>

struct Foo {
    Foo() : x(0) {}
    int x;
};


int main(){
    std::cout << "current PTHREAD_KEYS_MAX: " << PTHREAD_KEYS_MAX << std::endl;

    butil::DoublyBufferedData<Foo> data[PTHREAD_KEYS_MAX + 1];
    butil::DoublyBufferedData<Foo>::ScopedPtr ptr;

    return 1;
}


