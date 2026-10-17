// Must NOT compile under -Wthread-safety -Werror: Read() touches a guarded member without holding
// its mutex. tests/CMakeLists.txt registers it as a test that passes only when the compiler
// rejects it. guarded_access_good.cc is this file with the lock taken, so the two differ in
// exactly the line the analysis is meant to catch.
#include "util/mutex.h"
#include "util/thread_annotations.h"

namespace shale {

class Counter {
 public:
  int Read() { return x_; }

 private:
  Mutex mu_;
  int x_ SHALE_GUARDED_BY(mu_) = 0;
};

}  // namespace shale
