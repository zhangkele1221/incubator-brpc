#include<iostream>

#include <pthread.h>
#include <atomic>
#include <vector>



class Void { };

template <typename T, typename TLS = Void>
class DoublyBufferedData {
    class Wrapper;
    class WrapperTLSGroup;
    typedef int WrapperTLSId;
public:
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

    const T* UnsafeRead() const
    { return _data + _index.load(std::memory_order_acquire); }
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



// Use pthread_key store data limits by _SC_THREAD_KEYS_MAX.
// WrapperTLSGroup can store Wrapper in thread local storage.
// WrapperTLSGroup will destruct Wrapper data when thread exits,
// other times only reset Wrapper inner structure.
template <typename T, typename TLS>
class DoublyBufferedData<T, TLS>::WrapperTLSGroup {// 这个类型毛事没有 构造函数 自定义的 里面基本都是 static 函数和成员变量
public:
    const static size_t RAW_BLOCK_SIZE = 4096;
    const static size_t ELEMENTS_PER_BLOCK = (RAW_BLOCK_SIZE + sizeof(T) - 1) / sizeof(T);

    struct BAIDU_CACHELINE_ALIGNMENT ThreadBlock {
        inline DoublyBufferedData::Wrapper* at(size_t offset) {
            return _data + offset;
        };

    private:
        DoublyBufferedData::Wrapper _data[ELEMENTS_PER_BLOCK];
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
            butil::thread_atexit(_destroy_tls_blocks);
        }
        const size_t block_id = (size_t)id / ELEMENTS_PER_BLOCK;
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
        return tb->at(id - block_id * ELEMENTS_PER_BLOCK);
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
    static __thread std::vector<ThreadBlock*>* _s_tls_blocks;
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







template <typename T, typename TLS>
int DoublyBufferedData<T, TLS>::Read(typename DoublyBufferedData<T, TLS>::ScopedPtr* ptr) {
    Wrapper* p = WrapperTLSGroup::get_or_create_tls_data(_wrapper_key);
    Wrapper* w = AddWrapper(p);
    if (BAIDU_LIKELY(w != NULL)) {
        w->BeginRead();
        ptr->_data = UnsafeRead();
        ptr->_w = w;
        return 0;
    }
    return -1;
}




