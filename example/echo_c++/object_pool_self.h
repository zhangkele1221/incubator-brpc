
// 关键看懂 这个链接中的一张图 就大概掌握了  整个代码 https://blog.csdn.net/KIDGIN7439/article/details/120219052

#include <algorithm> // std::max, std::min
#include <iostream>  // std::ostream
#include <pthread.h> // pthread_mutex_t

#include <atomic>

//#include "butil/atomicops.h"              // butil::atomic
//#include "butil/macros.h"                 // BAIDU_CACHELINE_ALIGNMENT
//#include "butil/scoped_lock.h"            // BAIDU_SCOPED_LOCK
//#include "butil/thread_local.h"           // BAIDU_THREAD_LOCAL

#include <vector>

//#define BAIDU_THREAD_LOCAL __thread

#define BAIDU_LIKELY(expr) (expr)
#define BAIDU_UNLIKELY(expr) (expr)

#define BAIDU_CACHELINE_SIZE 64
#define BAIDU_CACHELINE_ALIGNMENT __attribute__((aligned(BAIDU_CACHELINE_SIZE)))

namespace butil {

template <typename T> inline T *get_object() {
  return ObjectPool<T>::singleton()->get_object();
}

template <typename T, typename A1> inline T *get_object(const A1 &arg1) {
  return ObjectPool<T>::singleton()->get_object(arg1);
}

template <typename T, typename A1, typename A2>
inline T *get_object(const A1 &arg1, const A2 &arg2) {
  return ObjectPool<T>::singleton()->get_object(arg1, arg2);
}

template <typename T> inline int return_object(T *ptr) {
  return ObjectPool<T>::singleton()->return_object(ptr);
}

template <typename T> inline void clear_objects() {
  ObjectPool<T>::singleton()->clear_objects();
}

template <typename T> ObjectPoolInfo describe_objects() {
  return ObjectPool<T>::singleton()->describe_objects();
}



template <typename T, size_t NITEM> struct ObjectPoolFreeChunk {
  size_t nfree;//表示可用对象的数量。
  T *ptrs[NITEM];//一个大小为 NITEM 的数组，存储指向类型 T 的对象的指针。
};
// for gcc 3.4.5
template <typename T> struct ObjectPoolFreeChunk<T, 0> {//在这个特化版本中，NITEM 为 0，因此 ptrs 数组的大小为 0。这个特化版本是为了解决某些编译器在处理大小为 0 的数组时可能出现的问题。
  size_t nfree;
  T *ptrs[0];
};

struct ObjectPoolInfo {
  size_t local_pool_num;
  size_t block_group_num;
  size_t block_num;
  size_t item_num;
  size_t block_item_num;
  size_t free_chunk_item_num;
  size_t total_size;
};

static const size_t OP_MAX_BLOCK_NGROUP = 65536;
static const size_t OP_GROUP_NBLOCK_NBIT = 16;
static const size_t OP_GROUP_NBLOCK = (1UL << OP_GROUP_NBLOCK_NBIT);//65536
static const size_t OP_INITIAL_FREE_LIST_SIZE = 1024;


template <typename T> struct ObjectPoolBlockMaxSize {//定义了每个块的最大字节大小，这里设置为 64 * 1024 字节（即 64KB）
    static const size_t value = 64 * 1024; // bytes
};
template <typename T> struct ObjectPoolBlockMaxItem {//定义了每个块中最多可以容纳的项目数量，这里设置为 256 个
    static const size_t value = 256;
};

template <typename T> class ObjectPoolBlockItemNum {//计算类，用于确定每个块实际包含的项目数量。它根据以下步骤计算：
  static const size_t N1 = ObjectPoolBlockMaxSize<T>::value / sizeof(T);//这个值 表示块的最大字节大小可以容纳多少个对象。
  static const size_t N2 = (N1 < 1 ? 1 : N1);//这样可以确保每个块至少包含一个对象。

public:
  static const size_t value =
      (N2 > ObjectPoolBlockMaxItem<T>::value ? ObjectPoolBlockMaxItem<T>::value
                                             : N2);//如果 N2 大于最大项目数，则将 value 设置为最大项目数，否则将 value 设置为 N2。这个值表示每个块实际应包含的项目数量。
};

template <typename T> class BAIDU_CACHELINE_ALIGNMENT ObjectPool {
public:
  static const size_t BLOCK_NITEM = ObjectPoolBlockItemNum<T>::value;//这个值表示每个块实际应包含的项目数量。
  static const size_t FREE_CHUNK_NITEM = BLOCK_NITEM;// 256 

  // Free objects are batched in a FreeChunk before they're added to
  // global list(_free_chunks).
  typedef ObjectPoolFreeChunk<T, FREE_CHUNK_NITEM> FreeChunk;
  typedef ObjectPoolFreeChunk<T, 0> DynamicFreeChunk;

  // When a thread needs memory, it allocates a Block. To improve locality,
  // items in the Block are only used by the thread.
  // To support cache-aligned objects, align Block.items by cacheline.
  struct BAIDU_CACHELINE_ALIGNMENT Block {
    char items[sizeof(T) * BLOCK_NITEM];//每个Block 都是一块连续内存 256*T 个这样的字节 就是 256个T
    size_t nitem;

    Block() : nitem(0) {}
  };

  // An Object addresses at most OP_MAX_BLOCK_NGROUP BlockGroups,
  // each BlockGroup addresses at most OP_GROUP_NBLOCK blocks. So an
  // object addresses at most OP_MAX_BLOCK_NGROUP * OP_GROUP_NBLOCK Blocks.
  struct BlockGroup {
      std::atomic<size_t> nblock;
      std::atomic<Block*> blocks[OP_GROUP_NBLOCK];

      BlockGroup() : nblock(0) {
          // We fetch_add nblock in add_block() before setting the entry,
          // thus address_resource() may sees the unset entry. Initialize
          // all entries to NULL makes such address_resource() return NULL.
          memset(static_cast<void*>(blocks), 0, sizeof(std::atomic<Block*>) * OP_GROUP_NBLOCK);
      }
  };


  class BAIDU_CACHELINE_ALIGNMENT LocalPool {
  public:
    explicit LocalPool(ObjectPool *pool)
        : _pool(pool), _cur_block(NULL), _cur_block_index(0) {
      _cur_free.nfree = 0;
    }

    ~LocalPool() {
      // Add to global _free if there're some free objects
      if (_cur_free.nfree) {
        _pool->push_free_chunk(_cur_free);
      }

      _pool->clear_from_destructor_of_local_pool();
    }

    static void delete_local_pool(void *arg) { delete (LocalPool *)arg; }

    // We need following macro to construct T with different CTOR_ARGS
    // which may include parenthesis because when T is POD, "new T()"
    // and "new T" are different: former one sets all fields to 0 which
    // we don't want.
#define BAIDU_OBJECT_POOL_GET(CTOR_ARGS)                                       \
  /* Fetch local free ptr */                                                   \
  if (_cur_free.nfree) {                                                       \
    return _cur_free.ptrs[--_cur_free.nfree];                                  \
  }                                                                            \
  /* Fetch a FreeChunk from global.                                            \
     TODO: Popping from _free needs to copy a FreeChunk which is               \
     costly, but hardly impacts amortized performance. */                      \
  if (_pool->pop_free_chunk(_cur_free)) {                                      \
    return _cur_free.ptrs[--_cur_free.nfree];                                  \
  }                                                                            \
  /* Fetch memory from local block */                                          \
  if (_cur_block && _cur_block->nitem < BLOCK_NITEM) {                         \
    T *obj = new ((T *)_cur_block->items + _cur_block->nitem) T CTOR_ARGS;     \
    if (!ObjectPoolValidator<T>::validate(obj)) {                              \
      obj->~T();                                                               \
      return NULL;                                                             \
    }                                                                          \
    ++_cur_block->nitem;                                                       \
    return obj;                                                                \
  }                                                                            \
  
  //去 _cur_block 中看有没有空闲，如果有则直接placement new即可，因为初始化的时候 _cur_block 是null，因此也拿不到
  //此时就会去全局新建一个Block赋给 _cur_block 然后从cur_block里创建TestClass返回
  /* Fetch a Block from global */                                              \
  _cur_block = add_block(&_cur_block_index);                                   \
  if (_cur_block != NULL) {                                                    \
    // 这个就叫 placemen new 指定位置 new  就在我之前分配的那块空间上new 对象
    T *obj = new ((T *)_cur_block->items + _cur_block->nitem) T CTOR_ARGS;     \
    if (!ObjectPoolValidator<T>::validate(obj)) {                              \
      obj->~T();                                                               \
      return NULL;                                                             \
    }                                                                          \
    ++_cur_block->nitem;                                                       \
    return obj;                                                                \
  }                                                                            \
  return NULL;

    inline T *get() { BAIDU_OBJECT_POOL_GET(); }

    template <typename A1> inline T *get(const A1 &a1) {
      BAIDU_OBJECT_POOL_GET((a1));
    }

    template <typename A1, typename A2>
    inline T *get(const A1 &a1, const A2 &a2) {
      BAIDU_OBJECT_POOL_GET((a1, a2));
    }

#undef BAIDU_OBJECT_POOL_GET

    inline int return_object(T *ptr) {
      // Return to local free list
      if (_cur_free.nfree < ObjectPool::free_chunk_nitem()) {
        _cur_free.ptrs[_cur_free.nfree++] = ptr;
        BAIDU_OBJECT_POOL_FREE_ITEM_NUM_ADD1;
        return 0;
      }
      // Local free list is full, return it to global.
      // For copying issue, check comment in upper get()
      if (_pool->push_free_chunk(_cur_free)) {
        _cur_free.nfree = 1;
        _cur_free.ptrs[0] = ptr;
        BAIDU_OBJECT_POOL_FREE_ITEM_NUM_ADD1;
        return 0;
      }
      return -1;
    }

  private:
    ObjectPool *_pool;
    Block *_cur_block;
    size_t _cur_block_index;
    FreeChunk _cur_free;
    pthread_key_t key;
  };


    // Create a Block and append it to right-most BlockGroup.
    static Block* add_block(size_t* index) {
        Block* const new_block = new(std::nothrow) Block;//首先，尝试创建一个新的 Block 对象，将其地址赋值给 new_block 如果创建失败（内存分配失败），则返回 NULL。
        if (NULL == new_block) {
            return NULL;
        }
        size_t ngroup;//第一次的时候 _ngroup == 0 ;
        do {
            ngroup = _ngroup.load(std::memory_order_acquire);//a. 从 _ngroup 原子变量中加载当前的 block 组数量 ngroup，使用 std::memory_order_acquire 内存顺序模型。
            if (ngroup >= 1) {//如果 ngroup 大于等于 1，获取最后一个 block 组 g。
                BlockGroup* const g =
                    _block_groups[ngroup - 1].load(std::memory_order_consume);//获取 add_block_group 创建的 BlockGroup
                const size_t block_index =
                    g->nblock.fetch_add(1, std::memory_order_relaxed);//使用 fetch_add 函数为当前 block 组的 nblock 原子变量增加 1，并将结果存储在 block_index 中。使用 std::memory_order_relaxed 内存顺序模型。
                if (block_index < OP_GROUP_NBLOCK) {//如果 block_index 小于 OP_GROUP_NBLOCK (表示当前 block 组中还有可用空间），将新创建的 new_block 存储到 block 组的 blocks[block_index] 中，使用 std::memory_order_release 内存顺序模型。然后，计算新 Block 的全局索引，并将其存储在 *index 中，最后返回新创建的 Block。
                    g->blocks[block_index].store(
                        new_block, std::memory_order_release);
                    *index = (ngroup - 1) * OP_GROUP_NBLOCK + block_index;//计算新 Block 的全局索引，并将其存储在 *index 中
                    return new_block;
                }
                g->nblock.fetch_sub(1, std::memory_order_relaxed);// 如果 block_index 不满足条件（即当前 block 组已满），使用 fetch_sub 函数将 block 组的 nblock 原子变量减 1，使用 std::memory_order_relaxed 内存顺序模型。
            }
        } while (add_block_group(ngroup));//ngroup == 0 会走这里

        // Fail to add_block_group.
        delete new_block;
        return NULL;
    }

    // Create a BlockGroup and append it to _block_groups.
    // Shall be called infrequently because a BlockGroup is pretty big.
    static bool add_block_group(size_t old_ngroup) {//这里就是会 new 一个 BlockGroup _ngroup 也会加 1
        BlockGroup* bg = NULL;
        //BAIDU_SCOPED_LOCK(_block_group_mutex);
        std::lock_guard<std::mutex> scoped_locker_dummy_at_line_43(_block_group_mutex);
        const size_t ngroup = _ngroup.load(butil::memory_order_acquire);
        if (ngroup != old_ngroup) {
            // Other thread got lock and added group before this thread.
            return true;
        }
        if (ngroup < OP_MAX_BLOCK_NGROUP) {
            bg = new(std::nothrow) BlockGroup;
            if (NULL != bg) {
                // Release fence is paired with consume fence in add_block()
                // to avoid un-constructed bg to be seen by other threads.
                _block_groups[ngroup].store(bg, butil::memory_order_release);
                _ngroup.store(ngroup + 1, butil::memory_order_release);
            }
        }
        return bg != NULL;
    }

  inline LocalPool *get_or_new_local_pool() {//通过这个函数，可以确保每个线程都有一个与之关联的 LocalPool 实例，并且在线程退出时正确地删除这些实例。
    LocalPool *lp = _local_pool;
    if (BAIDU_LIKELY(lp != NULL)) {
      return lp;
    }
    lp = new (std::nothrow) LocalPool(this);
    if (NULL == lp) {
      return NULL;
    }
    // BAIDU_SCOPED_LOCK(_change_thread_mutex); //avoid race with clear()
    // 替换成下面的容易理解
    std::lock_guard<std::mutex> scoped_locker_dummy_at_line_42(
        _change_thread_mutex);
    _local_pool = lp;
    //butil::thread_atexit(LocalPool::delete_local_pool, lp);//线程退出时候 处理的一些任务 pthrea的库中有类似的函数  以便在当前线程退出时自动删除关联的 LocalPool 实例
    // 这两个pthread 代替上面 thread_atexit
    pthread_setspecific(key, lp);
    // 在程序的某个地方初始化键，并提供清理函数。
    pthread_key_create(&key, LocalPool::delete_local_pool);
   //_nlocal.fetch_add(1, butil::memory_order_relaxed);//使用 fetch_add 函数原子地递增 _nlocal 成员变量的值。butil::memory_order_relaxed 表示使用松散的内存顺序，这意味着在递增 _nlocal 时不需要保证其他内存操作的顺序。
    _nlocal.fetch_add(1, std::memory_order_relaxed);
    return lp;
  }

  inline T *get_object() {
    LocalPool *lp = get_or_new_local_pool();
    if (BAIDU_LIKELY(lp != NULL)) {
      return lp->get();
    }
    return NULL;
  }


    static inline ObjectPool* singleton() {
        ObjectPool* p = _singleton.load(std::memory_order_consume);
        if (p) {
            return p;
        }
        pthread_mutex_lock(&_singleton_mutex);
        p = _singleton.load(std::memory_order_consume);
        if (!p) {
            p = new ObjectPool();
            _singleton.store(p, std::memory_order_release);
        }
        pthread_mutex_unlock(&_singleton_mutex);
        return p;
    }

private:
    ObjectPool() {
        _free_chunks.reserve(OP_INITIAL_FREE_LIST_SIZE);// 1024
        pthread_mutex_init(&_free_chunks_mutex, NULL);
    }

    ~ObjectPool() {
        pthread_mutex_destroy(&_free_chunks_mutex);
    }



    bool pop_free_chunk(FreeChunk& c) {// free_chunks 初始化时是空，只reserve了一个空间 所以 pop_free_chunk 也会失败，当不为空时候，会拿到一个 free_chunk 然后拷贝到LocalPool的cur_free中
        // Critical for the case that most return_object are called in
        // different threads of get_object.
        if (_free_chunks.empty()) {
            return false;
        }
        pthread_mutex_lock(&_free_chunks_mutex);
        if (_free_chunks.empty()) {
            pthread_mutex_unlock(&_free_chunks_mutex);
            return false;
        }
        DynamicFreeChunk* p = _free_chunks.back();
        _free_chunks.pop_back();
        pthread_mutex_unlock(&_free_chunks_mutex);
        c.nfree = p->nfree;
        memcpy(c.ptrs, p->ptrs, sizeof(*p->ptrs) * p->nfree);//当不为空时候，会拿到一个 free_chunk 然后拷贝到LocalPool的 _cur_free 中
        free(p);
        return true;
    }

  static std::atomic<ObjectPool*> _singleton;
  static pthread_mutex_t _singleton_mutex;

  static std::atomic<long> _nlocal;
  static __thread LocalPool* _local_pool;
  static std::atomic<size_t> _ngroup;

  static pthread_mutex_t _change_thread_mutex;
  static pthread_mutex_t _block_group_mutex;

  static std::atomic<BlockGroup*> _block_groups[OP_MAX_BLOCK_NGROUP];//OP_MAX_BLOCK_NGROUP == 65535

  std::vector<DynamicFreeChunk*> _free_chunks;//一个存储空闲对象块的向量  其元素类型为指向 DynamicFreeChunk 类型的指针。这个向量用于存储空闲的动态内存块，即不再使用的 DynamicFreeChunk 对象。这样在需要新的空闲块时，可以从这个向量中获取，而不是分配新的内存。同样，在释放内存块时，可以将它们添加回这个向量，以便将来重用。这有助于减少内存分配和释放的开销。
  pthread_mutex_t _free_chunks_mutex;//互斥锁，用于保护 _free_chunks 的访问

};


template <typename T>
__thread typename ObjectPool<T>::LocalPool*
ObjectPool<T>::_local_pool = NULL;

template <typename T>
std::atomic<ObjectPool<T>*> ObjectPool<T>::_singleton = NULL;


template <typename T>
std::atomic<long> ObjectPool<T>::_nlocal = 0;

template <typename T>
std::atomic<size_t> ObjectPool<T>::_ngroup = 0;

template <typename T>
pthread_mutex_t ObjectPool<T>::_singleton_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T>
pthread_mutex_t ObjectPool<T>::_block_group_mutex = PTHREAD_MUTEX_INITIALIZER;

template <typename T>
pthread_mutex_t ObjectPool<T>::_change_thread_mutex = PTHREAD_MUTEX_INITIALIZER;


} // namespace butil
