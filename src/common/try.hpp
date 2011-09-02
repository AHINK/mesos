#ifndef __TRY_HPP__
#define __TRY_HPP__

#include <assert.h>

#include <string>


template <typename T>
class Try
{
public:
  static Try<T> some(const T& t)
  {
    return Try<T>(SOME, new T(t));
  }

  static Try<T> error(const std::string& message)
  {
    return Try<T>(ERROR, NULL, message);
  }

  Try(const T& _t) : state(SOME), t(new T(_t)) {}

  Try(const Try<T>& that)
  {
    state = that.state;
    if (that.t != NULL) {
      t = new T(*that.t);
    } else {
      t = NULL;
    }
    message = that.message;
  }

  ~Try()
  {
    delete t;
  }

  Try<T>& operator = (const Try<T>& that)
  {
    if (this != &that) {
      state = that.state;
      if (that.t != NULL) {
        t = new T(*that.t);
      } else {
        t = NULL;
      }
      message = that.message;
    }

    return *this;
  }

  bool isSome() const { return state == SOME; }
  bool isError() const { return state == ERROR; }

  T get() const { assert(state == SOME); return *t; }

  std::string error() const { assert(state == ERROR); return message; }

private:
  enum State {
    SOME,
    ERROR
  };

  Try(State _state, T* _t = NULL, const std::string& _message = "")
    : state(_state), t(_t), message(_message) {}

  State state;
  T* t;
  std::string message;
};

#endif // __TRY_HPP__
