

#include "butil/object_pool_inl.h"

#include <algorithm> // std::max, std::min
#include <iostream>  // std::ostream
#include <pthread.h> // pthread_mutex_t

//#include "butil/atomicops.h"              // butil::atomic
//#include "butil/macros.h"                 // BAIDU_CACHELINE_ALIGNMENT
//#include "butil/scoped_lock.h"            // BAIDU_SCOPED_LOCK
//#include "butil/thread_local.h"           // BAIDU_THREAD_LOCAL

#include <vector>

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
  size_t nfree;
  T *ptrs[NITEM];
};
// for gcc 3.4.5
template <typename T> struct ObjectPoolFreeChunk<T, 0> {
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
static const size_t OP_GROUP_NBLOCK = (1UL << OP_GROUP_NBLOCK_NBIT);
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
  static const size_t FREE_CHUNK_NITEM = BLOCK_NITEM;

  // Free objects are batched in a FreeChunk before they're added to
  // global list(_free_chunks).
  typedef ObjectPoolFreeChunk<T, FREE_CHUNK_NITEM> FreeChunk;
  typedef ObjectPoolFreeChunk<T, 0> DynamicFreeChunk;

  // When a thread needs memory, it allocates a Block. To improve locality,
  // items in the Block are only used by the thread.
  // To support cache-aligned objects, align Block.items by cacheline.
  struct BAIDU_CACHELINE_ALIGNMENT Block {
    char items[sizeof(T) * BLOCK_NITEM];
    size_t nitem;

    Block() : nitem(0) {}
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
    BAIDU_OBJECT_POOL_FREE_ITEM_NUM_SUB1;                                      \
    return _cur_free.ptrs[--_cur_free.nfree];                                  \
  }                                                                            \
  /* Fetch a FreeChunk from global.                                            \
     TODO: Popping from _free needs to copy a FreeChunk which is               \
     costly, but hardly impacts amortized performance. */                      \
  if (_pool->pop_free_chunk(_cur_free)) {                                      \
    BAIDU_OBJECT_POOL_FREE_ITEM_NUM_SUB1;                                      \
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
  /* Fetch a Block from global */                                              \
  _cur_block = add_block(&_cur_block_index);                                   \
  if (_cur_block != NULL) {                                                    \
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
  };

  inline LocalPool *get_or_new_local_pool() {
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
    butil::thread_atexit(LocalPool::delete_local_pool, lp);
    _nlocal.fetch_add(1, butil::memory_order_relaxed);
    return lp;
  }

  inline T *get_object() {
    LocalPool *lp = get_or_new_local_pool();
    if (BAIDU_LIKELY(lp != NULL)) {
      return lp->get();
    }
    return NULL;
  }

  static pthread_mutex_t _change_thread_mutex;
};

} // namespace butil
