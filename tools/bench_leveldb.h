#ifndef SHALE_TOOLS_BENCH_LEVELDB_H_
#define SHALE_TOOLS_BENCH_LEVELDB_H_

// The LevelDB engine of shale_bench, which the comparison in docs/benchmarks.md measures shale
// against. It is built only where LevelDB is installed and the benchmarks are asked for, and only
// shale_bench and the test of the engine include this header, so that no other target needs
// LevelDB.

#include <leveldb/env.h>

#include <memory>

#include "bench_engine.h"

namespace shale {

/// Returns an engine that drives LevelDB, opened with the settings that put it level with shale's
/// defaults: a write buffer of 4 MiB, blocks of 4096 bytes, a Bloom filter of 10 bits a key, a
/// block cache of 8 MiB, 1000 open files, no compression, and checksums verified on every read. A
/// write is synced exactly when BenchEngine::Put() is asked to sync it, through LevelDB's own sync.
/// It reports no write amplification: WriteAmplification() is always "n/a".
/// @param env the Env that LevelDB does its file work through; null means the default one. It must
///   outlive the engine.
std::unique_ptr<BenchEngine> NewLevelDBBenchEngine(leveldb::Env* env = nullptr);

}  // namespace shale

#endif  // SHALE_TOOLS_BENCH_LEVELDB_H_
