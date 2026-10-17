// Must compile under -Wthread-safety -Werror. Identical to guarded_access_bad.cc except that Read()
// holds the mutex; it proves that the flags and include path used for the bad file are good, so
// that the bad file is rejected for the analysis and not for some unrelated error.
#include "util/mutex.h"
#include "util/thread_annotations.h"

namespace shale {

class Counter {
 public:
  int Read() {
    MutexLock lock(&mu_);
    return x_;
  }

 private:
  Mutex mu_;
  int x_ SHALE_GUARDED_BY(mu_) = 0;
};

}  // namespace shale
