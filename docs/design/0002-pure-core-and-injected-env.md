# 0002. A pure core, and every effect injected

Status: accepted

## Context

The hard bugs in a storage engine are in what happens between a crash and the next open: a file
renamed but its directory not yet synced, a log cut mid-record, a compaction that dropped an entry a
snapshot still needed. They are found by running the engine over very many crashes and comparing it
with a model, and they are only worth finding if the run that found one can be run again. A bug that
appeared once under a real kernel, a real thread schedule and a real clock is a story, not a test.

## Decision

Make a whole run a function of one integer. Two mechanisms do it.

**A pure kernel, `shale_core`.** The decisions of the engine that can be made without touching the
world are made by functions that touch nothing: encoding and decoding version edits, applying an edit
to the table set with every invariant checked (`ApplyEdit`), choosing the files of the next compaction
(`PickCompaction`), and deciding which entries of a compaction's input survive it
(`CompactionFilter`). The directory has no `<thread>`, `<mutex>`, `<atomic>`, `<chrono>`, `<fstream>`,
`<cstdio>`, `<iostream>`, `<future>` or `<random>`, and `shale_core` links `shale_util` and nothing else,
so a source that reached for the I/O layer would fail to link.

**Injected `Env` and `Scheduler`.** Everything else reaches files, directories, locks and the clock only
through `Env` (`include/shale/env.h`), and starts no thread of its own: it hands background work to a
`Scheduler` (`include/shale/scheduler.h`). Production uses `PosixEnv` and a worker thread. The
simulator uses:

- `FaultInjectionEnv` (`tests/testkit/fault_env.h`), an in-memory filesystem with a virtual clock that
  moves only when something sleeps. It models the gap between what a program wrote and what a disk
  kept: bytes in a handle's buffer are lost by any crash; bytes handed to the "OS" survive a process
  crash; a power loss keeps the bytes that were synced and a seeded prefix of the rest; each
  directory's unsynced changes (create, remove, rename) are kept as a seeded in-order prefix; a
  `Sync()` that failed marks the file so that later syncs of it succeed and make nothing durable. It
  can fail any call, and flip a bit in what is read.
- `ManualScheduler`, which runs a task only when its owner says so, on the owner's thread. A flush and a
  compaction are single steps of the simulator, so *when* they happen is a choice of the seed.
- `shale::Random` (`src/util/random.h`) with an explicit seed for everything else: skip-list heights,
  fault injection, operation generation. `std::rand`, `std::random_device` and `<random>` itself are
  banned in `src/`, `tests/` and `tools/`, because a distribution is implementation defined, and a seed
  would then not mean the same run under libstdc++ and libc++.

What enforces this: the link dependencies keep `shale_core` from reaching the I/O layer;
`scripts/check-layering.sh` reads every `#include`, fails on `throw` under `include/` and `src/`, and fails on
`std::rand`, `std::srand`, `rand(`, `srand(`, `std::random_device` and `#include <random>` under `src/`,
`tests/` and `tools/` (its own test shows each of these going red). That there is **no mutable global
state** is a rule that no tool checks, and that review holds: `Env::Default()` is a function-local static
that holds none.

## Consequences

**What this buys.**

- A failing seed is a reproduction: `SHALE_SIM_SEED=2240` runs the same operations, the same crashes and
  the same fault the next day. The seeds that once failed are kept in
  `tests/sim/regression_seeds.txt` and run in every build.
- `tests/sim/determinism_test.cc` asserts that one seed run twice gives the same trace digest, and pins
  the digests of three seeds, so a toolchain whose runs diverge from the others fails CI. That test
  exists because of a bug the simulator found: the arena padded to
  `alignof(std::max_align_t)`, which is 8 on one toolchain and 16 on another, so one seed filled a
  memtable one write earlier on one of them (commit [`b082176`][b082176]).
- The core can be tested against oracles in isolation, and its tests link nothing else.

**What it costs.**

- **An indirection on every I/O.** Calls go through a virtual `Env`. A change that wants `mmap`, direct
  I/O or `io_uring` has to grow the interface, and the simulated filesystem with it.
- **The simulator checks a model, not a machine.** What it finds is true of the model of the disk written
  in `fault_env.h`, and that file also lists what the model leaves out: torn, zero-filled and
  garbage-tailed files after a power loss (a power loss there leaves a clean prefix), the durability of
  `CreateDir`, a partial effect of a failed call, and bits that change at rest. See
  [testing](../testing.md#what-the-tests-do-not-cover).
- **No thread interleavings in the simulator.** `ManualScheduler` is single-threaded by design. Races are
  looked for elsewhere: a stress test under ThreadSanitizer and Clang's thread-safety analysis
  ([ADR 0006](0006-concurrency-model.md)).
- **State is a value.** `VersionState` is copied by `ApplyEdit` rather than changed in place, so every
  edit costs time proportional to the number of files in the table set. That is the price of making
  `ApplyEdit` a function that can be checked and replayed, and a flush or a compaction, which writes
  whole files, can afford it.
- **A crash happens between operations, never inside one.** The simulator reaches a crash in the middle of
  a flush or a compaction only by arming a failure of a call inside it, and by the reopen that follows.

## Alternatives considered

- **Intercept system calls** (an `LD_PRELOAD` shim, `ptrace`, or a FUSE filesystem). Rejected: the
  schedule of the kernel and of threads stays outside the seed, so a failure would not replay, and each
  is platform-specific.
- **Fault injection on a real filesystem** (a device-mapper target that drops writes, or killing a VM).
  Rejected for the simulator: it needs privileges and one OS, and a run cannot be replayed. It is not
  given up, since `shale_crashtest` kills a real process on a real filesystem, but that is a different
  test with a different claim; see [testing](../testing.md).
- **Property-test the pure core only.** Rejected: the failures that matter are in the order of the I/O
  that surrounds it, which the core does not do.
- **Run the real thread scheduler under a controlled-schedule tool.** Rejected: a tool that explores
  thread interleavings of C++ would be one more dependency, and the engine has one background thread
  whose interaction with the foreground is small enough to state ([ADR 0006](0006-concurrency-model.md))
  and to stress.
- **A mock `Env` that returns canned errors.** Rejected as the only tool: a failure that is not at the
  point where it matters teaches nothing. The fault-injecting Env fails the *n*th call of a kind, at
  seeded points of a whole run; the unit tests also use Envs that fail one chosen call
  (`tests/db/hook_env.h`) for a single path.

## References

- `include/shale/env.h`, `include/shale/scheduler.h`, `tests/testkit/fault_env.h`,
  `tests/testkit/simulator.h`
- [architecture](../architecture.md#layers), [testing](../testing.md)
- Commits: [`b082176`][b082176] (arena padding), [`491be63`][491be63], [`6d1f3b9`][6d1f3b9].

[491be63]: https://github.com/jason-te-sde/shale/commit/491be63b8907881ca766af9cd90eed10e5eafe73
[6d1f3b9]: https://github.com/jason-te-sde/shale/commit/6d1f3b9af394dc13c77883c8f5e384fd7c23ccab
[b082176]: https://github.com/jason-te-sde/shale/commit/b0821763439d9306d7427686350ae8995f54d7d7
