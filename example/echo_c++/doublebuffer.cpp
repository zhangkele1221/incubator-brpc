

class Void {};

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


template <typename T, typename TLS>
class DoublyBufferedData<T, TLS>::Wrapper : public DoublyBufferedDataWrapperBase<T, TLS> {

  friend class DoublyBufferedData;

public:
  explicit Wrapper() : _control(NULL) { pthread_mutex_init(&_mutex, NULL); }

  ~Wrapper() {
    if (_control != NULL) {
      _control->RemoveWrapper(this);
    }
    pthread_mutex_destroy(&_mutex);
  }

  // _mutex will be locked by the calling pthread and DoublyBufferedData.
  // Most of the time, no modifications are done, so the mutex is
  // uncontended and fast.
  inline void BeginRead() { pthread_mutex_lock(&_mutex); }

  inline void EndRead() { pthread_mutex_unlock(&_mutex); }

  inline void WaitReadDone() { BAIDU_SCOPED_LOCK(_mutex); }

private:
  DoublyBufferedData *_control;
  pthread_mutex_t _mutex;
};
