#include<iostream>
#include<limits.h>

#include "../../src/butil/DoublyBufferedData.h"


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


