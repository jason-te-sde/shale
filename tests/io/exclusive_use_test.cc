#include "io/exclusive_use.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>

namespace shale {
namespace {

TEST(ExclusiveUseTest, ScopesOneAfterAnotherAreFine) {
  ExclusiveUse use;
  for (int i = 0; i < 3; i++) {
    ExclusiveUse::Scope scope(&use, "file");
  }
}

TEST(ExclusiveUseTest, TwoObjectsDoNotInterfere) {
  ExclusiveUse a;
  ExclusiveUse b;
  ExclusiveUse::Scope in_a(&a, "a");
  ExclusiveUse::Scope in_b(&b, "b");
}

TEST(ExclusiveUseTest, HandingOverToAnotherThreadIsFine) {
  // One thread after another is what group commit does with a log file: the rule is no overlap,
  // not one owner for life.
  ExclusiveUse use;
  {
    ExclusiveUse::Scope scope(&use, "file");
  }
  std::thread other([&use] { ExclusiveUse::Scope scope(&use, "file"); });
  other.join();
  ExclusiveUse::Scope scope(&use, "file");
}

TEST(ExclusiveUseDeathTest, AnOverlappingScopeAbortsAndNamesWhatWasUsed) {
  EXPECT_DEATH(
      {
        ExclusiveUse use;
        ExclusiveUse::Scope first(&use, "WritableFile /db/000007.log");
        ExclusiveUse::Scope second(&use, "WritableFile /db/000007.log");
      },
      "concurrent use of WritableFile /db/000007.log");
}

TEST(ExclusiveUseDeathTest, ASecondScopeOnAnotherThreadAborts) {
  EXPECT_DEATH(
      {
        ExclusiveUse use;
        ExclusiveUse::Scope first(&use, "file");
        std::thread other([&use] { ExclusiveUse::Scope second(&use, "file"); });
        other.join();
      },
      "concurrent use of file");
}

}  // namespace
}  // namespace shale
