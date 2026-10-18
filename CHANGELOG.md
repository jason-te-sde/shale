# Changelog

All notable changes to shale are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows
[Semantic Versioning](https://semver.org/) with the caveat in [RELEASING.md](RELEASING.md): until 1.0,
a minor release may change the API.

## [Unreleased]

## [0.1.0] - 2026-10-18

The first release.

### Added

- **The engine.** An LSM-tree key-value store in C++20 with no third-party runtime dependency: a
  write-ahead log, a skip-list memtable, immutable sorted tables with a Bloom filter and per-block
  checksums, leveled compaction with trivial moves, group commit, write slowdown and stop, and a sticky
  error after a failed background write.
- **The API** (`include/shale/`): `DB::Open`, `Put`, `Delete`, `Write(WriteBatch)`, `Get`,
  `NewIterator` (forward, with `Seek`), snapshots, `Flush`, `CompactRange`, `GetProperty` and
  `DestroyDB`; `Options`, `ReadOptions`, `WriteOptions` and `ValidateOptions`; `Status`; the `Env` and
  `Scheduler` interfaces through which every file, lock, clock reading and background task goes.
- **The durability contract**, D1 to D7 ([docs/design/0004-durability.md](docs/design/0004-durability.md)):
  a synced write survives a crash of the process and of the machine, a batch is atomic, a read never
  returns a wrong value, and a log that is damaged is not guessed at.
- **Recovery** that tells the torn tail of a crashed log from corruption, refuses the second by default
  (`paranoid_checks`) and, when told to be lenient, keeps what it did not apply as `.dropped` files. A log
  that begins after a gap in the sequence numbers, which a power loss leaves because a memtable switch
  does not sync the old log, is set aside the same way by any open; nothing acknowledged as synced is in it.
- **Tools**: `shalectl` (put, get, delete, scan, stats, flush, verify and the dumps of tables, logs and
  MANIFESTs), `shale_bench` (db_bench-style workloads, also run against LevelDB with `--engine=leveldb` for
  the comparison in the benchmarks) and `shale_crashtest` (a real process killed with
  `SIGKILL`).
- **Packaging**: a CMake package (`find_package(shale)`, target `shale::shale`), an example that is built
  against the installation in CI, and presets for development, release, sanitizers, coverage and fuzzing.
- **Tests**: unit tests; a deterministic simulator over a fault-injecting filesystem with a reference model
  and six invariants; six injected flaws that the simulator must catch; stress tests under
  ThreadSanitizer; six libFuzzer targets; the crash test; and tests that hold the documents'
  numbers to the code.
- **Documentation**: [README](README.md), [architecture](docs/architecture.md), the byte-level
  [format](docs/format.md), the [operations guide](docs/operations.md), [testing](docs/testing.md), the
  [benchmarks](docs/benchmarks.md) and seven [design records](docs/design/).

[Unreleased]: https://github.com/jason-te-sde/shale/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/jason-te-sde/shale/releases/tag/v0.1.0
