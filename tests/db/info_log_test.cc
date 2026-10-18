// The informational log: what a person reads after something went wrong. It must never be the
// reason something does, so a log that cannot be written is silent, not fatal.

#include "db/info_log.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "shale/env.h"
#include "testkit/fault_env.h"
#include "testkit/test_util.h"
#include "util/filename.h"

namespace shale {
namespace {

constexpr char kDb[] = "/db";

class InfoLogTest : public testing::Test {
 protected:
  InfoLogTest() : env_(5) { EXPECT_OK(env_.CreateDir(kDb)); }

  std::string Contents(const std::string& fname) { return env_.FileContents(fname); }

  // The number of lines in `text`.
  static size_t Lines(const std::string& text) {
    size_t lines = 0;
    for (const char c : text) {
      if (c == '\n') {
        lines++;
      }
    }
    return lines;
  }

  FaultInjectionEnv env_;
};

TEST_F(InfoLogTest, EachLineIsWrittenWithATimestamp) {
  InfoLog log(&env_, kDb);
  env_.SleepForMicroseconds(1500000 + 250);  // the Env's clock is virtual and starts at zero
  log.Log("first");
  log.Log("second");
  EXPECT_EQ(Contents(InfoLogFileName(kDb)),
            "1.500250 first\n"
            "1.500250 second\n");
}

TEST_F(InfoLogTest, ALineIsVisibleAsSoonAsItIsLogged) {
  // Nobody reads the log through the object that writes it, so a line still in a buffer would
  // be lost with the process, and the lines before a crash are the ones that matter.
  InfoLog log(&env_, kDb);
  log.Log("before the crash");
  env_.SimulateProcessCrash();
  EXPECT_NE(Contents(InfoLogFileName(kDb)).find("before the crash"), std::string::npos);
}

TEST_F(InfoLogTest, ThePreviousLogBecomesLogOld) {
  {
    InfoLog first(&env_, kDb);
    first.Log("from the first run");
  }
  const std::string first_contents = Contents(InfoLogFileName(kDb));
  EXPECT_FALSE(env_.FileExists(OldInfoLogFileName(kDb)));

  InfoLog second(&env_, kDb);
  second.Log("from the second run");
  EXPECT_EQ(Contents(OldInfoLogFileName(kDb)), first_contents);
  const std::string current = Contents(InfoLogFileName(kDb));
  EXPECT_NE(current.find("from the second run"), std::string::npos);
  EXPECT_EQ(current.find("from the first run"), std::string::npos);
}

TEST_F(InfoLogTest, OnlyOneOldLogIsKept) {
  for (int run = 0; run < 4; run++) {
    InfoLog log(&env_, kDb);
    log.Log("run " + std::to_string(run));
  }
  std::vector<std::string> names;
  ASSERT_OK(env_.GetChildren(kDb, &names));
  EXPECT_EQ(names.size(), 2u);  // LOG and LOG.old
  EXPECT_NE(Contents(OldInfoLogFileName(kDb)).find("run 2"), std::string::npos);
  EXPECT_NE(Contents(InfoLogFileName(kDb)).find("run 3"), std::string::npos);
}

TEST_F(InfoLogTest, ALogThatCannotBeCreatedIsSilent) {
  env_.FailOpAfter(FaultInjectionEnv::Op::kNewWritable, 0);
  InfoLog log(&env_, kDb);
  log.Log("nowhere to go");  // does not fail, abort or create the file
  EXPECT_FALSE(env_.FileExists(InfoLogFileName(kDb)));
}

TEST_F(InfoLogTest, AFailedWriteStopsTheLogWithoutFailingTheCaller) {
  InfoLog log(&env_, kDb);
  log.Log("kept");
  env_.FailOpAfter(FaultInjectionEnv::Op::kAppend, 0);
  log.Log("lost");
  env_.ClearFaults();
  // The Env allows nothing but Close() after a failed write, so the log gives up for good.
  log.Log("also lost");
  const std::string contents = Contents(InfoLogFileName(kDb));
  EXPECT_NE(contents.find("kept"), std::string::npos);
  EXPECT_EQ(contents.find("lost"), std::string::npos);
}

TEST_F(InfoLogTest, ThreadsMayLogAtTheSameTime) {
  // A writable file aborts the process when two calls overlap, so the log serialises them.
  InfoLog log(&env_, kDb);
  constexpr int kThreads = 4;
  constexpr int kLinesEach = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&log, t] {
      for (int i = 0; i < kLinesEach; i++) {
        log.Log("thread " + std::to_string(t) + " line " + std::to_string(i));
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(Lines(Contents(InfoLogFileName(kDb))), size_t{kThreads * kLinesEach});
}

}  // namespace
}  // namespace shale
