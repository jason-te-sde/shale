#include "testkit/test_util.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "shale/iterator.h"
#include "util/check.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {

namespace {

class VectorIterator final : public Iterator {
 public:
  using Entries = std::vector<std::pair<std::string, std::string>>;

  VectorIterator(Entries entries, Status status)
      : entries_(std::move(entries)), status_(std::move(status)), position_(entries_.size()) {}

  bool Valid() const override { return position_ < entries_.size(); }

  void SeekToFirst() override { position_ = 0; }

  void Seek(std::string_view target) override {
    size_t low = 0;
    size_t high = entries_.size();
    while (low < high) {
      const size_t mid = low + (high - low) / 2;
      if (BytewiseCompare(entries_[mid].first, target) < 0) {
        low = mid + 1;
      } else {
        high = mid;
      }
    }
    position_ = low;
  }

  void Next() override {
    SHALE_CHECK(Valid(), "VectorIterator::Next requires Valid");
    ++position_;
  }

  std::string_view key() const override {
    SHALE_CHECK(Valid(), "VectorIterator::key requires Valid");
    return entries_[position_].first;
  }

  std::string_view value() const override {
    SHALE_CHECK(Valid(), "VectorIterator::value requires Valid");
    return entries_[position_].second;
  }

  Status status() const override { return status_; }

 private:
  const Entries entries_;
  const Status status_;
  size_t position_;  // entries_.size() when the iterator is not valid
};

}  // namespace

testing::AssertionResult IsOk(const Status& s) {
  if (s.ok()) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure() << s.ToString();
}

std::string RandomString(Random* rnd, size_t len) {
  std::string result(len, '\0');
  for (char& c : result) {
    c = static_cast<char>(' ' + rnd->Uniform(95));
  }
  return result;
}

std::string RandomBinaryString(Random* rnd, size_t len) {
  std::string result(len, '\0');
  for (char& c : result) {
    c = static_cast<char>(rnd->Uniform(256));
  }
  return result;
}

std::unique_ptr<Iterator> NewVectorIterator(std::vector<std::pair<std::string, std::string>> sorted,
                                            Status status) {
  return std::make_unique<VectorIterator>(std::move(sorted), std::move(status));
}

std::string NewTempDir(const std::string& prefix) {
  std::string pattern = testing::TempDir();
  if (pattern.empty() || pattern.back() != '/') {
    pattern.push_back('/');
  }
  pattern += prefix;
  pattern += "XXXXXX";
  // mkdtemp rewrites the template in place, so it needs a writable, NUL-terminated buffer.
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  const char* created = mkdtemp(buffer.data());
  SHALE_CHECK(created != nullptr, "mkdtemp failed for " + pattern + ": " + std::strerror(errno));
  return created;
}

}  // namespace shale
