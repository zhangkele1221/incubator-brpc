
#include <atomic>
#include <butil/logging.h>
#include <iostream>

//https://blog.csdn.net/wxj1992/article/details/104311730
//g++ steal_queue.h.cpp  -I/root/learing/incubator-brpc/output/include   -L/root/learing/incubator-brpc/output/lib -lbrpc  -o steal_queue 
// export LD_LIBRARY_PATH=/root/learing/incubator-brpc/output/lib:$LD_LIBRARY_PATH 


#define BAIDU_CACHELINE_SIZE 64
# define BAIDU_CACHELINE_ALIGNMENT __attribute__((aligned(BAIDU_CACHELINE_SIZE)))

template <typename T>
class WorkStealingQueue {
public:
    WorkStealingQueue()
        : _bottom(1)
        , _capacity(0)
        , _buffer(NULL)
        , _top(1) {
    }

    ~WorkStealingQueue() {
        delete [] _buffer;
        _buffer = NULL;
    }

    int init(size_t capacity) {
        if (_capacity != 0) {
            LOG(ERROR) << "Already initialized";
            return -1;
        }
        if (capacity == 0) {
            LOG(ERROR) << "Invalid capacity=" << capacity;
            return -1;
        }
        if (capacity & (capacity - 1)) {
            LOG(ERROR) << "Invalid capacity=" << capacity
                       << " which must be power of 2";
            return -1;
        }
        _buffer = new(std::nothrow) T[capacity];
        if (NULL == _buffer) {
            return -1;
        }
        _capacity = capacity;
        return 0;
    }

    // 将一个项推入队列。
    // 如果成功推入，则返回true。
    // 可能与 steal() 并行执行。
    // 绝不与pop()或另一个push()并行执行。
    bool push(const T& x) {
        const size_t b = _bottom.load(std::memory_order_relaxed);
        const size_t t = _top.load(std::memory_order_acquire);
        if (b >= t + _capacity) { // Full queue. == （_bottom - _top）>= _capacity 队列满了
            return false;
        }
        _buffer[b & (_capacity - 1)] = x;// b & (_capacity - 1) == b % _capacity 的一个快速等价物
        _bottom.store(b + 1, std::memory_order_release);
        return true;
    }

    // 从队列中弹出一个项。
    // 如果成功弹出，则返回true，并将项写入val。
    // 可能与steal()并行执行。
    // 绝不与push()或另一个pop()并行执行。
    bool pop(T* val) {
        const size_t b = _bottom.load(std::memory_order_relaxed);
        size_t t = _top.load(std::memory_order_relaxed);
        if (t >= b) {
            // 快速检查，因为我们在每个调度中调用了pop()。
            // 较小的陈旧的_top不应进入此分支。
            return false;
        }
        const size_t newb = b - 1;
        _bottom.store(newb, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        t = _top.load(std::memory_order_relaxed);
        if (t > newb) {
            _bottom.store(b, std::memory_order_relaxed);
            return false;
        }
        *val = _buffer[newb & (_capacity - 1)];
        if (t != newb) {
            return true;
        }
        // 单个最后一个元素，与steal()竞争。
        const bool popped = _top.compare_exchange_strong(
            t, t + 1, std::memory_order_seq_cst, std::memory_order_relaxed);
        _bottom.store(b, std::memory_order_relaxed);
        return popped;
    }
    // 底部下标值比上面的大.
    // 这个方法的目的是从队列的底部窃取任务，而不会与从队列的顶部弹出任务的其他线程发生冲突。请注意，此方法是线程安全的，并且可以与其他线程的push，pop和steal操作并行执行。
    // 从队列中窃取一个项。
    // 如果成功窃取，则返回true。
    // 可能与push()、pop()或另一个steal()并行执行。
    bool steal(T* val) {
        //加载队列的 _top 值，表示队列中可以被窃取的任务的起始位置。
        //这里使用的是memory_order_acquire内存序，这意味着在此操作之后的读操作不能被重排序到此操作之前。
        size_t t = _top.load(std::memory_order_acquire);
        //加载队列的_bottom值，表示队列的末尾，即最后一个被推入的任务的位置。这意味着在此操作之后的读操作不能被重排序到此操作之前。
        size_t b = _bottom.load(std::memory_order_acquire);
        //如果_top大于或等于_bottom，说明队列是空的或者没有可以被窃取的任务。在这种情况下，方法立即返回false。
        if (t >= b) {
            // 为了性能考虑，允许出现假负（false negative）。
            return false;
        }
        //是一个尝试窃取任务的循环
        do {
            //这是一个内存栅，它阻止了编译器和处理器对内存操作的重排序，以确保一致的内存顺序。
            std::atomic_thread_fence(std::memory_order_seq_cst);
            //再次加载 _bottom 的值，以检查是否有新任务被推入。
            b = _bottom.load(std::memory_order_acquire);
            if (t >= b) {
                return false;
            }
            //尝试从 _buffer 数组中的_top位置读取任务。这里使用&操作是为了快速地计算模运算。
            *val = _buffer[t & (_capacity - 1)];// t & (_capacity - 1) == t % _capacity 的一个快速等价物
            //这是一个原子操作，尝试将_top值增加1。
            //如果在此过程中_top的值没有改变，则操作成功，并且窃取任务成功，方法返回true。
            //如果_top的值已经被其他线程更改（即其他线程也在尝试窃取任务），则此操作失败，循环继续，直到窃取成功或队列为空。
        } while (!_top.compare_exchange_strong(t, t + 1,
                                               std::memory_order_seq_cst,
                                               std::memory_order_relaxed));
        //如果返回 true 这意味着 _top 的值与 t 相等，并且已成功地将 _top 的值设置为 t + 1
        //如果返回 false 这意味着 _top 的值与 t 不相等。此时，t 变量将被设置为 _top 的当前值，以便你可以选择重试或执行其他操作。
        return true;
    }

    size_t volatile_size() const {
        const size_t b = _bottom.load(std::memory_order_relaxed);
        const size_t t = _top.load(std::memory_order_relaxed);
        return (b <= t ? 0 : (b - t));
    }

    size_t capacity() const { return _capacity; }

    //DISALLOW_COPY_AND_ASSIGN 禁止复制构造和赋值
    // 禁止复制构造函数
    WorkStealingQueue(const WorkStealingQueue&) = delete;
    // 禁止赋值运算符函数
    WorkStealingQueue& operator=(const WorkStealingQueue&) = delete;

private:
    // Copying a concurrent structure makes no sense.
    //DISALLOW_COPY_AND_ASSIGN(WorkStealingQueue);

    std::atomic<size_t> _bottom;
    size_t _capacity;
    T* _buffer;
    std::atomic<size_t> BAIDU_CACHELINE_ALIGNMENT _top;
};

typedef size_t value_type;
bool g_stop = false;
const size_t N = 1024*512;
const size_t CAP = 8;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void* steal_thread(void* arg) {
    std::vector<value_type> *stolen = new std::vector<value_type>;
    stolen->reserve(N);
    WorkStealingQueue<value_type> *q = (WorkStealingQueue<value_type>*)arg;
    value_type val;
    while (!g_stop) {
        if (q->steal(&val)) {
            stolen->push_back(val);
        } else {
            asm volatile("pause\n": : :"memory");//是一个优化的暂停指令，通常用于多线程编程，特别是在自旋锁的情况下。通过使用 pause 指令，它可以提高性能和降低处理器的功耗。
        }
    }
    return stolen;
}

int main(){
    WorkStealingQueue<value_type> q;
    if(q.init(CAP) == 0){
        LOG(INFO) << "WorkStealingQueue init ok";
    }

    pthread_t rth[8];
    pthread_t wth, pop_th;
    for (size_t i = 0; i < ARRAY_SIZE(rth); ++i) {
        if(pthread_create(&rth[i], NULL, steal_thread, &q) == 0){
            LOG(INFO) <<"WorkStealingQueue pthread_create ok";
        }
    }


    return 1;
}


