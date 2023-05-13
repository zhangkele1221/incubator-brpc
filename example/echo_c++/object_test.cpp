
#include <iostream>
#include "butil/object_pool.h"

using namespace std;

using namespace butil;

struct MyObject {};


int main(){


    MyObject* p = get_object<MyObject>();
    std::cout << describe_objects<MyObject>() << std::endl;
    return_object(p);
    std::cout << describe_objects<MyObject>() << std::endl;


}



