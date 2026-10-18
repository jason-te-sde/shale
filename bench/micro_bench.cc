// Micro benchmarks of the pieces that the engine's hot paths are made of, one at a time: the
// checksum that every log record and block carries, the memtable's skip list, a seek inside a data
// block, and a Bloom filter probe. Each one's input is made from a fixed seed, so what differs
// between two runs is the machine. They say what a component costs, not what the database does:
// that is shale_bench's job.
//
// Each reports the time of one iteration, which is one checksum, one insert, one seek or one probe.
// None of them has a result that nothing reads: the value is passed to DoNotOptimize, so that the
// compiler cannot decide that the work is not needed.

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "io/block.h"
#include "io/block_builder.h"
#include "io/bloom.h"
#include "io/skiplist.h"
#include "shale/iterator.h"
#include "util/arena.h"
#include "util/crc32c.h"
#include "util/dbformat.h"
#include "util/random.h"

namespace shale {
namespace {

constexpr uint64_t kSeed = 301;

// The key of index `index`, as shale_bench makes them: sixteen digits.
std::string Key(uint64_t index) {
  std::array<char, 32> text{};
  std::snprintf(text.data(), text.size(), "%016llu", static_cast<unsigned long long>(index));
  return text.data();
}

// `size` bytes from the generator.
std::string RandomBytes(Random* random, size_t size) {
  std::string bytes(size, '\0');
  for (char& byte : bytes) {
    byte = static_cast<char>(random->Next());
  }
  return bytes;
}

// ---- CRC32C: the checksum of every log record and block. ----------------------------------------

void BM_Crc32c(benchmark::State& state) {
  const size_t size = static_cast<size_t>(state.range(0));
  Random random(kSeed);
  const std::string data = RandomBytes(&random, size);
  // Each checksum extends the one before, so that the loop depends on its own result and the
  // compiler cannot do the work once.
  uint32_t crc = 0;
  for ([[maybe_unused]] auto _ : state) {
    crc = crc32c::Extend(crc, data.data(), data.size());
    benchmark::DoNotOptimize(crc);
  }
  state.SetBytesProcessed(state.iterations() * state.range(0));
  state.SetLabel(crc32c::kHasHardwareSupport ? "hardware" : "portable");
}
BENCHMARK(BM_Crc32c)->Arg(4096);

// ---- The skip list under the memtable. ----------------------------------------------------------

struct Uint64Comparator {
  int operator()(const uint64_t& a, const uint64_t& b) const { return (a > b) - (a < b); }
};

using List = SkipList<uint64_t, Uint64Comparator>;

// The list starts empty again every this many inserts, so that a run of a million iterations is
// timed on lists of the size of a memtable, and not on one that keeps growing. This is about the
// number of entries that the default 4 MiB write buffer holds of 100-byte values. Freeing a list
// and its arena is in the time, spread over this many inserts.
constexpr uint64_t kInsertsPerList = 1 << 15;

void BM_SkipListInsert(benchmark::State& state) {
  Random keys(kSeed);
  auto arena = std::make_unique<Arena>();
  auto list = std::make_unique<List>(Uint64Comparator(), arena.get(), kSeed);
  uint64_t inserted = 0;
  for ([[maybe_unused]] auto _ : state) {
    if (inserted == kInsertsPerList) {
      // The generator's outputs are all different, which the list requires of its keys.
      list.reset();
      arena = std::make_unique<Arena>();
      list = std::make_unique<List>(Uint64Comparator(), arena.get(), kSeed);
      inserted = 0;
    }
    list->Insert(keys.Next());
    inserted++;
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SkipListInsert);

void BM_SkipListSeek(benchmark::State& state) {
  Random random(kSeed);
  Arena arena;
  List list(Uint64Comparator(), &arena, kSeed);
  for (uint64_t i = 0; i < kInsertsPerList; i++) {
    list.Insert(random.Next());
  }
  // Targets from the same generator, so that most of them are not in the list, and a seek lands on
  // the key after.
  std::vector<uint64_t> targets(1024);
  for (uint64_t& target : targets) {
    target = random.Next();
  }

  List::Iterator iterator(&list);
  size_t next = 0;
  for ([[maybe_unused]] auto _ : state) {
    iterator.Seek(targets[next]);
    next = (next + 1) % targets.size();
    benchmark::DoNotOptimize(iterator.Valid());
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SkipListSeek);

// ---- A seek inside a data block. ----------------------------------------------------------------

// Builds a data block as the table builder does: internal keys in order, 100-byte values, the
// default restart interval of 16, and entries added until it reaches the default block size of 4
// KiB. The user keys are the even numbers, so that a seek to an odd one lands on the key after it.
struct BlockFixture {
  std::shared_ptr<const Block> block;
  std::vector<std::string> targets;  // internal keys, half of them in the block
};

BlockFixture MakeBlockFixture() {
  constexpr size_t kBlockSize = 4096;
  constexpr int kRestartInterval = 16;
  Random random(kSeed);
  BlockBuilder builder(kRestartInterval);
  uint64_t entries = 0;
  while (builder.CurrentSizeEstimate() < kBlockSize) {
    std::string key;
    AppendInternalKey(&key, ParsedInternalKey{Key(entries * 2), 1, ValueType::kValue});
    builder.Add(key, RandomBytes(&random, 100));
    entries++;
  }

  BlockFixture fixture;
  fixture.block = std::make_shared<const Block>(std::string(builder.Finish()));
  for (int i = 0; i < 1024; i++) {
    std::string target;
    // As a point lookup builds its key: the newest sequence number, so that the seek lands on the
    // newest entry of the key.
    AppendInternalKey(&target,
                      ParsedInternalKey{Key(random.Uniform(static_cast<uint32_t>(entries * 2))),
                                        kMaxSequenceNumber, kValueTypeForSeek});
    fixture.targets.push_back(std::move(target));
  }
  return fixture;
}

void BM_BlockSeek(benchmark::State& state) {
  const BlockFixture fixture = MakeBlockFixture();
  const std::unique_ptr<Iterator> iterator =
      NewBlockIterator(fixture.block, CompareInternalKey, kInternalKeyTagSize);
  size_t next = 0;
  for ([[maybe_unused]] auto _ : state) {
    iterator->Seek(fixture.targets[next]);
    next = (next + 1) % fixture.targets.size();
    benchmark::DoNotOptimize(iterator->Valid());
  }
  state.SetItemsProcessed(state.iterations());
  state.SetLabel(std::to_string(fixture.block->size()) + " byte block");
}
BENCHMARK(BM_BlockSeek);

// ---- A Bloom filter probe. ----------------------------------------------------------------------

void BM_BloomMayMatch(benchmark::State& state) {
  constexpr int kBitsPerKey = 10;  // the default
  constexpr uint64_t kKeys = 16384;
  // The filter of a table of about two megabytes holds this many keys of the size that
  // shale_bench writes. It holds the even keys.
  BloomFilterBuilder builder(kBitsPerKey);
  for (uint64_t i = 0; i < kKeys; i++) {
    builder.AddKey(Key(i * 2));
  }
  const std::string filter = builder.Finish();

  // Probes alternate between a key that is in the filter and one that is not: the first costs all
  // the probes of the filter and answers yes, and the second stops at the first bit that is clear.
  Random random(kSeed);
  std::vector<std::string> probes;
  for (uint64_t i = 0; i < 1024; i++) {
    const uint64_t index = random.Uniform(static_cast<uint32_t>(kKeys));
    probes.push_back(Key(index * 2 + (i % 2)));
  }

  size_t next = 0;
  for ([[maybe_unused]] auto _ : state) {
    benchmark::DoNotOptimize(BloomFilterMayMatch(filter, probes[next]));
    next = (next + 1) % probes.size();
  }
  state.SetItemsProcessed(state.iterations());
  state.SetLabel("half the probes are of keys in the filter");
}
BENCHMARK(BM_BloomMayMatch);

}  // namespace
}  // namespace shale
