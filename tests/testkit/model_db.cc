#include "testkit/model_db.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "shale/status.h"
#include "shale/write_batch.h"
#include "util/check.h"
#include "util/coding.h"
#include "util/hash.h"

namespace shale {

namespace {

// Applies the operations of a batch to a state as they come.
class ApplyHandler final : public WriteBatch::Handler {
 public:
  explicit ApplyHandler(ModelDB::State* state) : state_(state) {}

  void Put(std::string_view key, std::string_view value) override {
    (*state_)[std::string(key)] = std::string(value);
  }

  void Delete(std::string_view key) override { state_->erase(std::string(key)); }

 private:
  ModelDB::State* const state_;
};

// The two chains of the digest start from different seeds, so that the 64 bits are two hashes of
// the same bytes and not one hash twice.
constexpr uint32_t kDigestSeedHigh = 0x6d6f6465;
constexpr uint32_t kDigestSeedLow = 0x7368616c;

}  // namespace

void ModelDB::Put(std::string key, std::string value) {
  state_.insert_or_assign(std::move(key), std::move(value));
}

void ModelDB::Delete(const std::string& key) { state_.erase(key); }

void ModelDB::Apply(const WriteBatch& batch) {
  ApplyHandler handler(&state_);
  const Status s = batch.Iterate(&handler);
  SHALE_CHECK(s.ok(), "ModelDB::Apply was given a malformed batch: " + s.ToString());
}

std::optional<std::string> ModelDB::Get(const std::string& key) const {
  const auto found = state_.find(key);
  if (found == state_.end()) {
    return std::nullopt;
  }
  return found->second;
}

const ModelDB::State& ModelDB::state() const { return state_; }

void ModelDB::Reset(State state) { state_ = std::move(state); }

uint64_t ModelDB::Digest(const State& state) {
  uint32_t high = kDigestSeedHigh;
  uint32_t low = kDigestSeedLow;
  const auto mix = [&high, &low](const char* data, size_t n) {
    high = Hash(data, n, high);
    low = Hash(data, n, low);
  };
  const auto mix_string = [&mix](const std::string& s) {
    char length[8];
    EncodeFixed64(length, s.size());
    mix(length, sizeof(length));
    mix(s.data(), s.size());
  };
  for (const auto& [key, value] : state) {
    mix_string(key);
    mix_string(value);
  }
  return (uint64_t{high} << 32) | low;
}

}  // namespace shale
