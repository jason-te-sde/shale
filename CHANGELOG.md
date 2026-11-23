# Changelog

All notable changes to shale are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows
[Semantic Versioning](https://semver.org/) with the caveat in [RELEASING.md](RELEASING.md): until 1.0,
a minor release may change the API.

## [Unreleased]

## [0.1.2] - 2026-11-23

No file the engine writes has changed, and the table format version is the same. The figures below are from [the 2026-11 measurement](docs/benchmarks.md#2026-11-the-read-path-012),
made on one Apple M2 in one session with v0.1.1 and 0.1.2 run in interleaved rounds; each is a median of three rounds.
A ratio above 1 means that 0.1.2 is faster.

### Changed

- **Every read row of `shale_bench` is faster than in v0.1.1**, in the first pass and after a compaction, by the rule
  of the protocol (the rounds of the two builds do not overlap and the medians are at least 3% apart). Ratios of
  0.1.2 to v0.1.1, first pass and compacted: `readseq` 1.97 and 1.98, `seekrandom` 1.24 and 1.25, `readmissing` 1.11 and
  1.16, `readrandom` 1.04 and 1.04
  ([results](docs/benchmarks.md#2026-11-the-read-rows-in-interleaved-rounds)).
- **A table is read through one iterator**, in place of a stack of five, and a compaction reads its input tables through
  it. It makes every check the old path made (the block checksum whenever `verify_checksums` is set, the type byte, key
  order, the bound of each index entry and the landing of a seek), and every error is the same and still names the file.
- **A table iterator reads ahead** once it has moved to the next data block twice in a row after a seek: one read takes
  the block and the 8 KiB after it, then twice as many each time, up to 256 KiB. A seek starts again and `Get()` never
  reads ahead. A table read in order costs up to 256 KiB plus one block of buffer. It cut the `pread` calls of
  `readseq` from 31,786 to 875 in the first pass and from 27,711 to 765 after a compaction. There is no option for it.
- **A read allocates less.** A `Get` whose block is cached allocates 1 time against 6 in v0.1.1, a `Get` that reads
  its block from the file 2 against 8, and a seek whose blocks are cached 9 against 39 (libc++, 16-byte keys, as
  `read_allocations_test` counts them; [figures](docs/benchmarks.md#2026-11-allocations-per-operation)).
- **Compaction is faster.** A compaction of the whole key range of the same files took 2.593 s in v0.1.1 and 1.509 s
  in 0.1.2, with the same compactions and the same bytes read and written
  ([figures](docs/benchmarks.md#2026-11-e-made-compaction-faster)). After `overwrite`, 0.1.2 has made all 50 of its
  compactions when the last write returns, where v0.1.1 has made 37 or 38.

### Added

- `shale_bench --cache_size=BYTES` sets the block cache of the shale engine, from 0 to 64 GiB (default 8 MiB). A run with
  any other size prints a `# DIAGNOSTIC` line and says `"diagnostic": true` in its JSON, because it is not a row of the
  main comparison.
- `shale_bench --existing_db=WRITE` opens the database at `--db` as it is and runs only reads on it, checked against
  what the write benchmark `WRITE` makes. After every write run `shale_bench` now waits until the engine's background
  is idle, and a write row's JSON gives its figures when the last write returned and once settled.

## [0.1.1] - 2026-10-20

### Fixed

- A database could refuse to open after a process kill on macOS, when the operating system left the
  MANIFEST ending in zero bytes; a zero-filled tail from a record boundary is now tolerated as a crash
  trace (D6). Zeros that start at a record boundary and run to the end of a write-ahead log or the
  MANIFEST are dropped whatever `paranoid_checks` says, once the reader has checked every byte to the
  end of the file, and the `LOG` names the file and the number of zero bytes. Zeros followed by any other
  byte, zeros that begin inside a record that the file holds in full, and garbage tails are defects, as
  before. The simulator's power
  loss now leaves such zeros too ([durability](docs/design/0004-durability.md),
  [operations](docs/operations.md#a-database-that-will-not-open)).

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

[Unreleased]: https://github.com/jason-te-sde/shale/compare/v0.1.2...HEAD
[0.1.2]: https://github.com/jason-te-sde/shale/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/jason-te-sde/shale/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/jason-te-sde/shale/releases/tag/v0.1.0
