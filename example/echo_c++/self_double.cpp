#include<iostream>

#include <pthread.h>
#include <atomic>
#include <vector>
#include <deque>


class Void { };

template <typename T, typename TLS = Void>
class DoublyBufferedData {
    class Wrapper;
    class WrapperTLSGroup;
    typedef int WrapperTLSId;
public:
    /*
        ScopedPtr可以理解为一个带锁的指针，
        通过Read函数读取数据后会把当前前台数据的指针和当前线程的 wrapper 指针保存到ScopedPtr里，
        可以通过它来获取数据和用户侧tls变量，并且通过wrapper进行同步，一旦使用ScopedPtr发起了读，
        只要ScopedPtr还存活，当前wrapper就是上锁的状态，因为解锁在析构函数里，这可以确保读取过程当前在读取的数据不被修改。
    */
    class ScopedPtr {
    friend class DoublyBufferedData;
    public:
        ScopedPtr() : _data(NULL), _w(NULL) {}
        ~ScopedPtr() {
            if (_w) {
                _w->EndRead();
            }
        }
        const T* get() const { return _data; }
        const T& operator*() const { return *_data; }
        const T* operator->() const { return _data; }
        TLS& tls() { return _w->user_tls(); }
        
    private:
        DISALLOW_COPY_AND_ASSIGN(ScopedPtr);
        const T* _data;
        Wrapper* _w;
    };
    
    DoublyBufferedData();
    ~DoublyBufferedData();

    // Put foreground instance into ptr. The instance will not be changed until
    // ptr is destructed.
    // This function is not blocked by Read() and Modify() in other threads.
    // Returns 0 on success, -1 otherwise.
    int Read(ScopedPtr* ptr);

    // Modify background and foreground instances. fn(T&, ...) will be called
    // twice. Modify() from different threads are exclusive from each other.
    // NOTE: Call same series of fn to different equivalent instances should
    // result in equivalent instances, otherwise foreground and background
    // instance will be inconsistent.
    template <typename Fn> size_t Modify(Fn& fn);
    template <typename Fn, typename Arg1> size_t Modify(Fn& fn, const Arg1&);
    template <typename Fn, typename Arg1, typename Arg2>
    size_t Modify(Fn& fn, const Arg1&, const Arg2&);

    // fn(T& background, const T& foreground, ...) will be called to background
    // and foreground instances respectively.
    template <typename Fn> size_t ModifyWithForeground(Fn& fn);
    template <typename Fn, typename Arg1>
    size_t ModifyWithForeground(Fn& fn, const Arg1&);
    template <typename Fn, typename Arg1, typename Arg2>
    size_t ModifyWithForeground(Fn& fn, const Arg1&, const Arg2&);
    
private:
    template <typename Fn>
    struct WithFG0 {
        WithFG0(Fn& fn, T* data) : _fn(fn), _data(data) { }
        size_t operator()(T& bg) {
            return _fn(bg, (const T&)_data[&bg == _data]);
        }
    private:
        Fn& _fn;
        T* _data;
    };

    template <typename Fn, typename Arg1>
    struct WithFG1 {
        WithFG1(Fn& fn, T* data, const Arg1& arg1)
            : _fn(fn), _data(data), _arg1(arg1) {}
        size_t operator()(T& bg) {
            return _fn(bg, (const T&)_data[&bg == _data], _arg1);
        }
    private:
        Fn& _fn;
        T* _data;
        const Arg1& _arg1;
    };

    template <typename Fn, typename Arg1, typename Arg2>
    struct WithFG2 {
        WithFG2(Fn& fn, T* data, const Arg1& arg1, const Arg2& arg2)
            : _fn(fn), _data(data), _arg1(arg1), _arg2(arg2) {}
        size_t operator()(T& bg) {
            return _fn(bg, (const T&)_data[&bg == _data], _arg1, _arg2);
        }
    private:
        Fn& _fn;
        T* _data;
        const Arg1& _arg1;
        const Arg2& _arg2;
    };

    template <typename Fn, typename Arg1>
    struct Closure1 {
        Closure1(Fn& fn, const Arg1& arg1) : _fn(fn), _arg1(arg1) {}
        size_t operator()(T& bg) { return _fn(bg, _arg1); }
    private:
        Fn& _fn;
        const Arg1& _arg1;
    };

    template <typename Fn, typename Arg1, typename Arg2>
    struct Closure2 {
        Closure2(Fn& fn, const Arg1& arg1, const Arg2& arg2)
            : _fn(fn), _arg1(arg1), _arg2(arg2) {}
        size_t operator()(T& bg) { return _fn(bg, _arg1, _arg2); }
    private:
        Fn& _fn;
        const Arg1& _arg1;
        const Arg2& _arg2;
    };

    const T* UnsafeRead() const{ 
        return _data + _index.load(std::memory_order_acquire); 
    }
    Wrapper* AddWrapper(Wrapper*);
    void RemoveWrapper(Wrapper*);

    // Foreground and background void.
    T _data[2];//前台后台两份数据

    // Index of foreground instance.
    std::atomic<int> _index;//指向前台的index

    // Key to access thread-local wrappers.
    WrapperTLSId _wrapper_key;

    // All thread-local instances.
    std::vector<Wrapper*> _wrappers;//_wrappers 则是包含了所有的thread local数据，包括读锁和用户tls

    // Sequence access to _wrappers.
    pthread_mutex_t _wrappers_mutex;

    // Sequence modifications.
    pthread_mutex_t _modify_mutex;
};




//构造函数主要是初始化互斥锁和tls key，并且由于数组并不会默认初始化元素，因此_data需要手动初始化，确保初始值符合预期 _wrapper_key
template <typename T, typename TLS>
DoublyBufferedData<T, TLS>::DoublyBufferedData()
    : _index(0)
    , _wrapper_key(0) {
    _wrappers.reserve(64);
    pthread_mutex_init(&_modify_mutex, NULL);
    pthread_mutex_init(&_wrappers_mutex, NULL);
    _wrapper_key = WrapperTLSGroup::key_create();
    // Initialize _data for some POD types. This is essential for pointer
    // types because they should be Read() as NULL before any Modify().
    if (is_integral<T>::value || is_floating_point<T>::value ||
        is_pointer<T>::value || is_member_function_pointer<T>::value) {
        _data[0] = T();
        _data[1] = T();
    }
}

//析构函数则是销毁相关互斥锁和删除tls key，并逐个删除各个线程创建的tls数据。
template <typename T, typename TLS>
DoublyBufferedData<T, TLS>::~DoublyBufferedData() {
    // User is responsible for synchronizations between Read()/Modify() and
    // this function.
    
    {
        BAIDU_SCOPED_LOCK(_wrappers_mutex);
        for (size_t i = 0; i < _wrappers.size(); ++i) {
            _wrappers[i]->_control = NULL;  // hack: disable removal.
        }
        _wrappers.clear();
    }
    WrapperTLSGroup::key_delete(_wrapper_key);
    _wrapper_key = -1;
    pthread_mutex_destroy(&_modify_mutex);
    pthread_mutex_destroy(&_wrappers_mutex);
}

// Called when thread initializes thread-local wrapper.
template <typename T, typename TLS>
typename DoublyBufferedData<T, TLS>::Wrapper* 
DoublyBufferedData<T, TLS>::AddWrapper(typename DoublyBufferedData<T, TLS>::Wrapper* w) {
    if (NULL == w) {
        return NULL;
    }
    if (w->_control == this) {//这什么意思 没有搞明白
        return w;
    }
    if (w->_control != NULL) {
        LOG(FATAL) << "Get wrapper from tls but control != this";
        return NULL;
    }
    try {
        w->_control = this;
        BAIDU_SCOPED_LOCK(_wrappers_mutex);
        _wrappers.push_back(w);
    } catch (std::exception& e) {
        return NULL;
    }
    return w;
}


// 使用 pthread_key 存储数据受到 _SC_THREAD_KEYS_MAX 的限制。
// WrapperTLSGroup 可以在线程本地存储中存储 Wrapper 
// 当线程退出时 WrapperTLSGroup 将销毁 Wrapper 数据，
// 其他时间只重置 Wrapper 内部结构。"
template <typename T, typename TLS>
class DoublyBufferedData<T, TLS>::WrapperTLSGroup {  // 这个类型毛事没有 构造函数 自定义的 里面基本都是 static 函数和成员变量
public:
    const static size_t RAW_BLOCK_SIZE = 4096;//RAW_BLOCK_SIZE 定义了原始块的大小，通常以字节为单位。在这个例子中，它是 4096 字节
    //RAW_BLOCK_SIZE + sizeof(T) - 1：加上 sizeof(T) - 1 是一个常见的技巧，
    //用于确保当 RAW_BLOCK_SIZE 不是 sizeof(T) 的整数倍时，结果会向上取整到最接近的 sizeof(T) 的倍数。
    //这是通过整数除法的截断行为来实现的。(RAW_BLOCK_SIZE + sizeof(T) - 1) / sizeof(T)：
    //最后，通过除以 sizeof(T) 来计算每个块可以容纳的元素数量。由于这是整数除法，结果会自动向下取整。
    // 这种计算的目的是确保每个块的大小（以字节为单位）是类型 T 大小的整数倍，以避免内存对齐问题。
    // 举个简单的例子，假设 RAW_BLOCK_SIZE 是 10，sizeof(T) 是 4。
    //如果你简单地做 10 / 4，你得到2，这意味着你可能会浪费一些空间。
    //使用这个技巧，ELEMENTS_PER_BLOCK 会计算为 (10 + 4 - 1) / 4，即 13 / 4，结果是3，这样就没有浪费空间，并且可以更有效地使用内存块。
    const static size_t ELEMENTS_PER_BLOCK = (RAW_BLOCK_SIZE + sizeof(T) - 1) / sizeof(T);//常量 表示每个块可以容纳的元素数量。

    struct BAIDU_CACHELINE_ALIGNMENT ThreadBlock {
        inline DoublyBufferedData::Wrapper* at(size_t offset) {
            return _data + offset;
        };

    private:
        DoublyBufferedData::Wrapper _data[ELEMENTS_PER_BLOCK];//表示每个块种的元素数量
    };

    inline static WrapperTLSId key_create() {
        BAIDU_SCOPED_LOCK(_s_mutex);
        WrapperTLSId id = 0;
        if (!_get_free_ids().empty()) {
            id = _get_free_ids().back();
            _get_free_ids().pop_back();
        } else {
            id = _s_id++;
        }
        return id;
    }

    inline static int key_delete(WrapperTLSId id) {
        BAIDU_SCOPED_LOCK(_s_mutex);
        if (id < 0 || id >= _s_id) {
            errno = EINVAL;
            return -1;
        }
        _get_free_ids().push_back(id);
        return 0;
    }

    inline static DoublyBufferedData::Wrapper* get_or_create_tls_data(WrapperTLSId id) {
        if (BAIDU_UNLIKELY(id < 0)) {
            CHECK(false) << "Invalid id=" << id;
            return NULL;
        }
        if (_s_tls_blocks == NULL) {
            _s_tls_blocks = new (std::nothrow) std::vector<ThreadBlock*>;
            if (BAIDU_UNLIKELY(_s_tls_blocks == NULL)) {
                LOG(FATAL) << "Fail to create vector, " << berror();
                return NULL;
            }
            butil::thread_atexit(_destroy_tls_blocks);//该函数在线程退出时被调用，以清理线程局部存储
        }
        const size_t block_id = (size_t)id / ELEMENTS_PER_BLOCK;//计算给定的ID应该位于哪个块。这是通过简单地将ID除以每个块的元素数量来实现的。因为这里使用的是整数除法，结果会自动向下取整。
        if (block_id >= _s_tls_blocks->size()) {
            // The 32ul avoid pointless small resizes.
            _s_tls_blocks->resize(std::max(block_id + 1, 32ul));
        }
        ThreadBlock* tb = (*_s_tls_blocks)[block_id];
        if (tb == NULL) {
            ThreadBlock* new_block = new (std::nothrow) ThreadBlock;
            if (BAIDU_UNLIKELY(new_block == NULL)) {
                return NULL;
            }
            tb = new_block;
            (*_s_tls_blocks)[block_id] = new_block;
        }
        // 整个数据结构是由多个块组成的， （__thread std::vector<ThreadBlock*>* _s_tls_blocks;） 
        // 每个块包含 ELEMENTS_PER_BLOCK 个元素。（ DoublyBufferedData::Wrapper _data[ELEMENTS_PER_BLOCK];）
        // 给定一个 id，我们可以找出它属于哪个块（block_id），以及在那个块内的位置。
        // 还是那句话 id 是个全局的 索引
        return tb->at(id - block_id * ELEMENTS_PER_BLOCK);//用id -  块id*块容量    计算了在块内的索引位置
    }

private:
    static void _destroy_tls_blocks() {
        if (!_s_tls_blocks) {
            return;
        }
        for (size_t i = 0; i < _s_tls_blocks->size(); ++i) {
            delete (*_s_tls_blocks)[i];
        }
        delete _s_tls_blocks;
        _s_tls_blocks = NULL;
    }

    inline static std::deque<WrapperTLSId>& _get_free_ids() {
        if (BAIDU_UNLIKELY(!_s_free_ids)) {
            _s_free_ids = new (std::nothrow) std::deque<WrapperTLSId>();
            if (!_s_free_ids) {
                abort();
            }
        }
        return *_s_free_ids;
    }

private:
    static pthread_mutex_t _s_mutex;
    static WrapperTLSId _s_id;
    static std::deque<WrapperTLSId>* _s_free_ids;
    static __thread std::vector<ThreadBlock*>* _s_tls_blocks;//每个线程 独有的
};


template <typename T, typename TLS>
pthread_mutex_t DoublyBufferedData<T, TLS>::WrapperTLSGroup::_s_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T, typename TLS>
std::deque<typename DoublyBufferedData<T, TLS>::WrapperTLSId>*
        DoublyBufferedData<T, TLS>::WrapperTLSGroup::_s_free_ids = NULL;

template <typename T, typename TLS>
typename DoublyBufferedData<T, TLS>::WrapperTLSId
        DoublyBufferedData<T, TLS>::WrapperTLSGroup::_s_id = 0;

template <typename T, typename TLS>
__thread std::vector<typename DoublyBufferedData<T, TLS>::WrapperTLSGroup::ThreadBlock*>*
        DoublyBufferedData<T, TLS>::WrapperTLSGroup::_s_tls_blocks = NULL;



/*
这段代码的意义在于，它定义了一个模板类 DoublyBufferedDataWrapperBase
这个类一般接受两个类型参数。但是，当第二个类型参数是特定的类型Void时，类的定义是特化的。
模板特化是模板编程中的一个重要概念，允许为模板的某些特定参数提供不同的实现。
在这个例子中，当 DoublyBufferedDataWrapperBase 的第二个模板参数是Void时，
类没有任何成员，而其他情况下，它有一个成员变量和一个成员函数。
*/
template <typename T, typename TLS> class DoublyBufferedDataWrapperBase {
public:
  TLS &user_tls() { return _user_tls; }

protected:
  TLS _user_tls;
};

template <typename T> class DoublyBufferedDataWrapperBase<T, Void> {};

//tls锁及用户数据Wrapper
//Wrapper继承自 DoublyBufferedDataWrapperBase 成员变量有两个，
//所属的DoublyBufferedData指针 _control 和一个互斥锁 _mutex。
//这个互斥锁在读的时候以及更新数据的时候都要用到。
//BeginRead()和EndRead()都是供读线程使用 BeginRead()在读数据之前被调用，
//EndRead()在ScopedPtr的析构函数里被调用 WaitReadDone()
//则是在修改的时候被修改线程调用，这几个函数都是对锁的操作。
template <typename T, typename TLS>
class DoublyBufferedData<T, TLS>::Wrapper : public DoublyBufferedDataWrapperBase<T, TLS> {

    friend class DoublyBufferedData;
    
    public:
    explicit Wrapper() : _control(NULL) {
        pthread_mutex_init(&_mutex, NULL);
    }
    
    ~Wrapper() {
        if (_control != NULL) {
            _control->RemoveWrapper(this);
        }
        pthread_mutex_destroy(&_mutex);
    }

    // _mutex 将被调用 pthread 和 DoublyBufferedData 锁定。
    // 大多数时候，没有进行任何修改，所以互斥体是
    // 无竞争且快速。
    inline void BeginRead() {
        pthread_mutex_lock(&_mutex);
    }

    inline void EndRead() {
        pthread_mutex_unlock(&_mutex);
    }

    inline void WaitReadDone() {
        BAIDU_SCOPED_LOCK(_mutex);
    }
    
private:
    DoublyBufferedData* _control;
    pthread_mutex_t _mutex;
};




//DBD的读基于ScopedPtr，读的时候首先去尝试获取tls的wrapper，
//如果没获取到，则需要新建并加入全局wrapper列表，获取到wrapper后，
//执行BeginRead加tls读锁，随后进行读取，Read只会加锁不会释放锁，ScopedPtr销毁的时候才会释放。
template <typename T, typename TLS>
int DoublyBufferedData<T, TLS>::Read(typename DoublyBufferedData<T, TLS>::ScopedPtr* ptr) {
    Wrapper* p = WrapperTLSGroup::get_or_create_tls_data(_wrapper_key);//这就是用_wrapper_key 找到 DoublyBufferedData::Wrapper _data[ELEMENTS_PER_BLOCK];的元素偏移地址
    Wrapper* w = AddWrapper(p);
    if (BAIDU_LIKELY(w != NULL)) {
        w->BeginRead();
        ptr->_data = UnsafeRead();
        ptr->_w = w;
        return 0;
    }
    return -1;
}


/*
首先是获得修改锁，然后调用fn修改后台数据，如果修改失败就直接返回了。
修改完之后反转index，这里用的是release内存序，可以保证读线程一旦读到了新的index，
数据的修改也一定可见，在这个操作之后，所有的read获得的就是修改后的新前台数据了，
然后依次调用所有wrapper的WaitReadDone（），其实就是挨个每获取完一个锁就释放，
确保在index反转之前的所有读取都已经结束，这个循环过后当前的后台数据就没有线程在用了，可以安全修改，再次调用fn
*/

//总的来说，这个 Modify 方法允许你在一个双缓冲的数据结构上安全地执行修改操作，而不会阻止读取操作。这在多线程环境中是非常有用的，尤其是当读取操作远远多于写入操作时。
template <typename T, typename TLS>
template <typename Fn>
size_t DoublyBufferedData<T, TLS>::Modify(Fn& fn) {
    //"_modify_mutex 用于对修改进行排序。
    //使用一个单独的互斥锁而不是 _wrappers_mutex 是为了避免阻塞那些调用 AddWrapper() 或 RemoveWrapper() 的线程过长时间。
    //大多数时候，修改是由一个线程完成的，竞争应该可以忽略不计。"
    BAIDU_SCOPED_LOCK(_modify_mutex); //以确保同一时间只有一个线程可以修改数据
    int bg_index = !_index.load(butil::memory_order_relaxed);//这一行计算后台缓冲区的索引。_index 通常是一个原子变量，存储当前正在使用的缓冲区（前景）的索引。背景缓冲区是另一个，我们通过简单地取反来计算它。
    //"背景实例不会被其他线程访问，因此可以安全地进行修改。"
    const size_t ret = fn(_data[bg_index]);//对背景缓冲区的数据执行修改，并存储返回的结果。fn 应该是一个可调用对象，接受数据类型 T 的引用作为参数。
    if (!ret) {
        return 0;
    }

    
    // "发布，交换背景和前景实例。释放栅栏与 UnsafeRead() 中的获取栅栏匹配，
    //以确保刚开始读取新的前景实例的读取者能够看到在 fn 中所做的所有更改。"
    // UnsafeRead  return _data + _index.load(std::memory_order_acquire);  这一点很重要.....
    _index.store(bg_index, butil::memory_order_release);//发布更改，将 _index 设置为背景缓冲区的索引，从而将其切换到前景。这里使用的内存顺序保证，在发布更改之前，对背景缓冲区的更改对其他线程是可见的。
    bg_index = !bg_index;//重新计算背景缓冲区的索引，因为我们刚刚切换了它们。
    
    //"等待直到所有线程完成当前的读取。当它们开始下一次读取时，它们应该能看到更新后的 _index "
    {
        BAIDU_SCOPED_LOCK(_wrappers_mutex);
        for (size_t i = 0; i < _wrappers.size(); ++i) {
            _wrappers[i]->WaitReadDone();
        }
    }

    const size_t ret2 = fn(_data[bg_index]);//再次调用函数对象 fn，对新的背景缓冲区的数据执行相同的修改。 说白了就是之前是前台的数据..
    //简单来说，这行代码是在确认两次修改操作（前后台切换前和切换后）的结果是否一致，并在不一致的情况下输出错误信息并终止程序。
    CHECK_EQ(ret2, ret) << "index=" << _index.load(butil::memory_order_relaxed);
    return ret2;
}


template <typename T, typename TLS>
template <typename Fn, typename Arg1>
size_t DoublyBufferedData<T, TLS>::Modify(Fn& fn, const Arg1& arg1) {
    Closure1<Fn, Arg1> c(fn, arg1);
    return Modify(c);
}








/*
1.修改没有使用std::function之类的新特性应该是考虑到兼容性。
2.如果先执行读后执行修改要小心死锁，因为ScopedPtr在读之后只要还在生命周期锁就不会释放，此时进行修改就会死锁。
3.tls锁存在的主要意义是前后台数据切换后能比较迅速地安全修改后台数据（没人在继续读已经切到后台的数据），另外一种常见方案是没有锁，sleep一段时间再修改，确保新后台不再被检索线程访问。
4.比较适合读取速度快的场景，如果读取时间很长修改很容易被阻塞。

https://blog.csdn.net/wxj1992/article/details/117454485
*/