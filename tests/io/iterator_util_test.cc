// The iterator helpers: the empty and error iterators, and the two wrappers that keep something
// alive, or run something, for as long as an iterator lives.
//
// What the wrappers have to get right is the order. An iterator may point into memory that the pin
// or the cleanup releases, so the wrapped iterator must be gone before either of them runs.

#include "io/iterator_util.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "io/block.h"
#include "io/block_builder.h"
#include "shale/iterator.h"
#include "shale/status.h"
#include "testkit/test_util.h"
#include "util/dbformat.h"

namespace shale {
namespace {

// An iterator over nothing that tells a callback when it is destroyed, so that a test can see what
// else has or has not happened by then.
class DestructionProbe final : public Iterator {
 public:
  explicit DestructionProbe(std::function<void()> on_destroy)
      : on_destroy_(std::move(on_destroy)) {}

  ~DestructionProbe() override { on_destroy_(); }

  bool Valid() const override { return false; }
  void SeekToFirst() override {}
  void Seek(std::string_view /*target*/) override {}
  void Next() override {}
  std::string_view key() const override { return {}; }
  std::string_view value() const override { return {}; }
  Status status() const override { return Status::OK(); }

 private:
  const std::function<void()> on_destroy_;
};

// An iterator over "a" -> "1", "b" -> "2", "c" -> "3" that the wrapper tests can look through.
std::unique_ptr<Iterator> NewThreeEntryIterator() {
  BlockBuilder builder(16);
  builder.Add("a", "1");
  builder.Add("b", "2");
  builder.Add("c", "3");
  return NewBlockIterator(std::make_shared<const Block>(std::string(builder.Finish())),
                          BytewiseCompare);
}

std::vector<std::string> Keys(Iterator* it) {
  std::vector<std::string> keys;
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    keys.emplace_back(it->key());
  }
  return keys;
}

TEST(IteratorUtilTest, EmptyAndError) {
  const std::unique_ptr<Iterator> empty = NewEmptyIterator();
  EXPECT_FALSE(empty->Valid()) << "before a seek";
  empty->SeekToFirst();
  EXPECT_FALSE(empty->Valid());
  EXPECT_OK(empty->status());
  empty->Seek("anything");
  EXPECT_FALSE(empty->Valid());
  EXPECT_OK(empty->status());

  const std::unique_ptr<Iterator> error = NewErrorIterator(Status::Corruption("x"));
  EXPECT_FALSE(error->Valid());
  error->SeekToFirst();
  EXPECT_FALSE(error->Valid());
  error->Seek("anything");
  EXPECT_FALSE(error->Valid());
  EXPECT_TRUE(error->status().IsCorruption());
  EXPECT_EQ(error->status().ToString(), Status::Corruption("x").ToString());
}

TEST(IteratorUtilTest, PinningKeepsObjectAlive) {
  std::shared_ptr<int> object = std::make_shared<int>(42);
  const std::weak_ptr<int> watcher = object;

  std::unique_ptr<Iterator> it = NewPinningIterator(NewEmptyIterator(), object);
  object.reset();
  EXPECT_FALSE(watcher.expired()) << "the wrapper holds the object";

  it.reset();
  EXPECT_TRUE(watcher.expired()) << "and lets go of it when it is destroyed";
}

TEST(IteratorUtilTest, PinningReleasesAfterTheWrappedIterator) {
  std::shared_ptr<int> object = std::make_shared<int>(42);
  const std::weak_ptr<int> watcher = object;
  bool pinned_when_inner_died = false;

  std::unique_ptr<Iterator> it = NewPinningIterator(
      std::make_unique<DestructionProbe>([&] { pinned_when_inner_died = !watcher.expired(); }),
      object);
  object.reset();
  it.reset();

  EXPECT_TRUE(pinned_when_inner_died) << "the wrapped iterator may point into the pinned object";
  EXPECT_TRUE(watcher.expired());
}

TEST(IteratorUtilTest, CleanupRunsOnDestroy) {
  int runs = 0;
  std::unique_ptr<Iterator> it = NewCleanupIterator(NewEmptyIterator(), [&runs] { ++runs; });
  EXPECT_EQ(runs, 0) << "not before the wrapper is destroyed";
  it->SeekToFirst();
  EXPECT_EQ(runs, 0) << "and not by using it";

  it.reset();
  EXPECT_EQ(runs, 1);
}

TEST(IteratorUtilTest, CleanupRunsAfterTheWrappedIterator) {
  std::vector<std::string> events;
  std::unique_ptr<Iterator> it = NewCleanupIterator(
      std::make_unique<DestructionProbe>([&events] { events.emplace_back("inner destroyed"); }),
      [&events] { events.emplace_back("cleanup"); });
  it.reset();

  EXPECT_EQ(events, (std::vector<std::string>{"inner destroyed", "cleanup"}))
      << "the cleanup may release what the wrapped iterator points into";
}

TEST(IteratorUtilTest, WrappersForwardEverything) {
  int runs = 0;
  std::unique_ptr<Iterator> it = NewCleanupIterator(
      NewPinningIterator(NewThreeEntryIterator(), std::make_shared<int>(1)), [&runs] { ++runs; });

  EXPECT_FALSE(it->Valid());
  EXPECT_EQ(Keys(it.get()), (std::vector<std::string>{"a", "b", "c"}));
  EXPECT_OK(it->status());

  it->Seek("b");
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "b");
  EXPECT_EQ(it->value(), "2");
  it->Next();
  ASSERT_TRUE(it->Valid());
  EXPECT_EQ(it->key(), "c");
  it->Next();
  EXPECT_FALSE(it->Valid());

  it->Seek("d");
  EXPECT_FALSE(it->Valid());
  EXPECT_EQ(runs, 0);
  it.reset();
  EXPECT_EQ(runs, 1);

  const std::unique_ptr<Iterator> failing =
      NewPinningIterator(NewErrorIterator(Status::Corruption("x")), std::make_shared<int>(1));
  EXPECT_TRUE(failing->status().IsCorruption()) << "an error passes through the wrapper";
}

}  // namespace
}  // namespace shale
