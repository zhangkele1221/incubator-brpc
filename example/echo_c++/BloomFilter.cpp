#include <vector>
#include <bitset>
#include <cmath>
#include <functional>
#include <iostream>
/*

这个简单的例子使用了标准库中的std::bitset和std::hash来实现一个Bloom过滤器。在实际应用中，根据数据量的大小和可接受的误报率，你可能需要选择不同的位数组大小和哈希函数数量。

这个Bloom过滤器的使用场景可能如下：

当用户输入一个搜索关键词时，搜索引擎首先使用Bloom过滤器快速检查这个词是否可能存在于索引中。
如果Bloom过滤器表示关键词可能存在，搜索引擎接下来会在倒排索引中进行详细的查找。
如果Bloom过滤器表示关键词绝对不存在，则搜索引擎可以立即返回结果，不必进行更多的索引查找操作。
请注意，这只是一个Bloom过滤器的非常简单的实现，真实的搜索引擎会有更复杂的需求和实现。例如，他们可能需要一个动态大小的Bloom过滤器，多个独立的哈希函数，以及更多的优化来处理大规模的数据集。


..................... 那么可以理解为bloom 是一个用cpu算力 置换空间的一个思想呢？ ...........

是的，您可以这样理解。

Bloom过滤器是一个经典的空间效率极高的数据结构，它使用一系列哈希函数和一个位数组来检测一个元素是否可能在一个集合中。在牺牲了一定的误报率（false positives）的情况下，它可以极大地减少需要存储的数据量，从而节省空间。

这种数据结构特别适用于那些对存储空间敏感，但可以接受一定误报率的场景。Bloom过滤器通过多个哈希函数计算出的索引将信息存储在位数组中，一个元素的存在性通过检查所有相关哈希函数计算出的位是否被设置来推断。

使用Bloom过滤器的优点包括：

空间效率：它可以用远小于实际数据大小的空间来表示一个大集合。
时间效率：检查元素是否存在的操作时间复杂度为O(k)，其中k是哈希函数的数量，通常是一个很小的常数。
易于合并：两个Bloom过滤器可以很容易地通过位运算合并成一个新的Bloom过滤器，而不需要访问原始数据。
不过，Bloom过滤器也有其局限性：

误报：它可能会错误地表示某个元素存在于集合中，尽管实际上它并不存在（false positive）。
不支持删除：传统的Bloom过滤器不支持从集合中删除元素，尽管有变种如Counting Bloom Filter可以做到这一点。
固定大小：一旦建立，Bloom过滤器的大小就是固定的，不能根据需要扩展，除非使用可扩展的Bloom过滤器变种。
在处理大数据集时，使用Bloom过滤器可以显著降低内存和存储需求，同时提供快速的查询响应时间，这是通过牺牲一定的CPU计算资源来实现的。




*/
class BloomFilter {
private:
    std::bitset<1000> bitArray; // 选择合适的大小，这里只是示例
    int hashCount; // 哈希函数的数量

public:
    BloomFilter(int hashes) : hashCount(hashes) {}

    // 一个简单的哈希函数，实际应用中可能需要更复杂的哈希函数来减少碰撞
    size_t hash(const std::string& key, int seed) {
        std::hash<std::string> hasher;
        return hasher(key + std::to_string(seed)) % bitArray.size();
    }

    // 向Bloom过滤器中添加元素
    void add(const std::string& key) {
        for (int i = 0; i < hashCount; ++i) {
            bitArray.set(hash(key, i));
        }
    }

    // 检查元素是否可能在集合中
    bool possiblyContains(const std::string& key) {
        for (int i = 0; i < hashCount; ++i) {
            if (!bitArray.test(hash(key, i))) {
                return false; // 如果有一个位不是1，则元素一定不在集合中
            }
        }
        return true; // 可能在集合中，但需要进一步检查
    }
};

int main() {
    BloomFilter filter(7); // 使用7个哈希函数

    // 添加一些关键词
    filter.add("apple");
    filter.add("banana");
    filter.add("orange");

    // 检查关键词是否可能存在
    if (filter.possiblyContains("banana")) {
        std::cout << "Banana might be in the set." << std::endl;
    } else {
        std::cout << "Banana is definitely not in the set." << std::endl;
    }

    if (filter.possiblyContains("grape")) {
        std::cout << "Grape might be in the set." << std::endl;
    } else {
        std::cout << "Grape is definitely not in the set." << std::endl;
    }

    return 0;
}
