#ifndef SHALE_TOOLS_BENCH_LEVELDB_H_
#define SHALE_TOOLS_BENCH_LEVELDB_H_

// The LevelDB engine of shale_bench, which the comparison in docs/benchmarks.md measures shale
// against. It is built only where LevelDB is installed and the benchmarks are asked for, and only
// shale_bench and the test of the engine include this header, so that no other target needs
// LevelDB.

#include <leveldb/env.h>

#include <cstdint>
#include <memory>
#include <vector>

#include "bench_engine.h"

namespace shale {

/// Returns an engine that drives LevelDB, opened with the settings that put it level with shale's
/// defaults: a write buffer of 4 MiB, blocks of 4096 bytes, a Bloom filter of 10 bits a key, a
/// block cache of 8 MiB, 1000 open files, no compression, and checksums verified on every read. A
/// write is synced exactly when BenchEngine::Put() is asked to sync it, through LevelDB's own sync.
/// It reports no write amplification: WriteAmplification() is always "n/a".
///
/// GetState() reads the tables of each level from the property "leveldb.sstables", and gives as
/// counters time_sec, read_mb and write_mb: the sums over the levels of the Time(sec), Read(MB) and
/// Write(MB) columns of "leveldb.stats", which LevelDB prints for each level as a whole number. The
/// background is busy while a task that LevelDB handed to its Env's Schedule() has not returned,
/// which the engine counts through an Env of its own around `env`.
/// @param env the Env that LevelDB does its file work through; null means the default one. It must
///   outlive the engine.
std::unique_ptr<BenchEngine> NewLevelDBBenchEngine(leveldb::Env* env = nullptr);

/// Returns the highest compaction score among LevelDB's levels by its own rule (VersionSet::
/// Finalize() in LevelDB 1.23, whose constants are compiled in): level 0 scores its files over 4,
/// each level below it its bytes over 10 MiB times 10 to the power level - 1, and the last level,
/// which has nowhere to compact into, is not counted. LevelDB compacts a level whose score is 1 or
/// more.
/// @param files_per_level the files of each level, from level 0.
/// @param bytes_per_level the bytes of each level, from level 0.
double LevelDBHighestScore(const std::vector<uint64_t>& files_per_level,
                           const std::vector<uint64_t>& bytes_per_level);

}  // namespace shale

#endif  // SHALE_TOOLS_BENCH_LEVELDB_H_
