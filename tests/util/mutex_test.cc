#include "util/mutex.h"

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "util/thread_annotations.h"

namespace shale {
namespace {

TEST(MutexTest, AssertHeldPassesWhenHeld) {
  Mutex mu;
  mu.Lock();
  mu.AssertHeld();
  mu.Unlock();
  {
    MutexLock lock(&mu);
    mu.AssertHeld();
  }
}

TEST(MutexTest, LockExcludesConcurrentWriters) {
  struct Counter {
    Mutex mu;
    int value SHALE_GUARDED_BY(mu) = 0;
  } counter;
  constexpr int kThreads = 4;
  constexpr int kIncrements = 20000;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; t++) {
    threads.emplace_back([&counter] {
      for (int i = 0; i < kIncrements; i++) {
        MutexLock lock(&counter.mu);
        counter.value++;
      }
    });
  }
  for (std::thread& t : threads) {
    t.join();
  }
  MutexLock lock(&counter.mu);
  EXPECT_EQ(counter.value, kThreads * kIncrements);
}

// A gate that `waiters` threads block on until the main thread opens it. The waiters count
// themselves in under the mutex and only release it inside Wait(), so a main thread that has seen
// all of them counted in knows every one is blocked in Wait() before it opens the gate.
struct Gate {
  Mutex mu;
  CondVar arrived{&mu};
  CondVar opened{&mu};
  int waiting SHALE_GUARDED_BY(mu) = 0;
  int woken SHALE_GUARDED_BY(mu) = 0;
  bool open SHALE_GUARDED_BY(mu) = false;
};

void WaitForGate(Gate* gate) {
  MutexLock lock(&gate->mu);
  gate->waiting++;
  gate->arrived.SignalAll();
  while (!gate->open) {
    gate->opened.Wait();
  }
  // Wait() reacquired the mutex on this thread, and the mutex knows whose it is again.
  gate->mu.AssertHeld();
  gate->woken++;
}

TEST(MutexTest, CondVarWakesWaiter) {
  Gate gate;
  std::thread waiter(WaitForGate, &gate);
  {
    MutexLock lock(&gate.mu);
    while (gate.waiting < 1) {
      gate.arrived.Wait();
    }
    gate.open = true;
    gate.opened.Signal();
  }
  waiter.join();
  MutexLock lock(&gate.mu);
  EXPECT_EQ(gate.woken, 1);
}

TEST(MutexTest, SignalAllWakesEveryWaiter) {
  constexpr int kWaiters = 3;
  Gate gate;
  std::vector<std::thread> waiters;
  waiters.reserve(kWaiters);
  for (int i = 0; i < kWaiters; i++) {
    waiters.emplace_back(WaitForGate, &gate);
  }
  {
    MutexLock lock(&gate.mu);
    while (gate.waiting < kWaiters) {
      gate.arrived.Wait();
    }
    gate.open = true;
    gate.opened.SignalAll();
  }
  for (std::thread& t : waiters) {
    t.join();
  }
  MutexLock lock(&gate.mu);
  EXPECT_EQ(gate.woken, kWaiters);
}

TEST(MutexDeathTest, AssertHeldFailsWhenNotHeld) {
  EXPECT_DEATH(
      {
        Mutex mu;
        mu.AssertHeld();
      },
      "not held");
}

TEST(MutexDeathTest, AssertHeldFailsAfterUnlock) {
  Mutex mu;
  mu.Lock();
  mu.Unlock();
  EXPECT_DEATH(mu.AssertHeld(), "not held");
}

TEST(MutexDeathTest, AssertHeldFailsOnAThreadThatDoesNotHoldIt) {
  Mutex mu;
  MutexLock lock(&mu);
  EXPECT_DEATH(
      {
        std::thread other([&mu] { mu.AssertHeld(); });
        other.join();
      },
      "not held");
}

// Deliberate misuse. Clang's analysis rejects both at compile time, which is its job; the run-time
// checks cover the paths it cannot see, so the tests switch the analysis off to reach them.
void LockTwice(Mutex* mu) SHALE_NO_THREAD_SAFETY_ANALYSIS {
  mu->Lock();
  mu->Lock();
}

void UnlockWithoutLocking(Mutex* mu) SHALE_NO_THREAD_SAFETY_ANALYSIS { mu->Unlock(); }

TEST(MutexDeathTest, LockFailsWhenThisThreadAlreadyHoldsIt) {
  // Without the check this is a self-deadlock, and the death test would hang instead of passing.
  Mutex mu;
  EXPECT_DEATH(LockTwice(&mu), "already held");
}

TEST(MutexDeathTest, UnlockFailsWhenNotHeld) {
  Mutex mu;
  EXPECT_DEATH(UnlockWithoutLocking(&mu), "not held");
}

TEST(MutexDeathTest, WaitFailsWithoutHoldingTheMutex) {
  EXPECT_DEATH(
      {
        Mutex mu;
        CondVar cv(&mu);
        cv.Wait();
      },
      "not held");
}

}  // namespace
}  // namespace shale
