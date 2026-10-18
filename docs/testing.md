# Testing

shale's claims are only as good as the tests that can break them, so this page says what each layer
of tests is for, how to run it, how to reproduce a failure, and, as plainly as it can, what is *not*
tested.

## Contents

- [The layers](#the-layers)
- [Running them](#running-them)
- [The simulator](#the-simulator)
- [Reproducing a seed](#reproducing-a-seed)
- [Adding a regression seed](#adding-a-regression-seed)
- [Injected flaws](#injected-flaws)
- [Stress](#stress)
- [Fuzzing](#fuzzing)
- [The real crash test](#the-real-crash-test)
- [Tests of the repository itself](#tests-of-the-repository-itself)
- [CI and preflight](#ci-and-preflight)
- [What the tests do not cover](#what-the-tests-do-not-cover)

## The layers

| Layer | What it is for | Where |
| --- | --- | --- |
| **Unit tests** | One module at a time. The decoders of what is read from a disk (coding, log reader, block, table, write batch, version edit) are tested with truncated input and with lengths and counts that run past the end, and must return `Corruption` and never crash or read outside the buffer; the log reader, block and table tests add damaged bytes of their own (every bit of a record's length, random bit flips, a flipped byte in a block), and the fuzz targets feed random bytes to the log reader, the block and table readers and the version edit decoder (and the varints it is made of), and, through the recovery of the table set, to the log reader again and to the edits of a MANIFEST. The write batch decoder is covered by `tests/io/write_batch_test.cc` alone. | `tests/util`, `tests/io`, `tests/core`, `tests/db` |
| **The simulator** | The whole engine over a fault-injecting filesystem and a manual scheduler, driven by a seed, checked against a reference model after **every** step. This is where crash consistency is shown. | `tests/sim`, `tests/testkit` |
| **Injected flaws** | Proof that the simulator can fail: six deliberate bugs, each of which must make it go red. | `scripts/run-flaws.sh`, the `flaws` CI job |
| **Stress** | The engine under real threads and ThreadSanitizer: writers, readers, scans and snapshots racing flushes and compactions. | `tests/stress` |
| **Sanitizers** | The whole suite under AddressSanitizer and UndefinedBehaviorSanitizer, and the suite under ThreadSanitizer, three times (the single-threaded simulator and its determinism test once, at 20 seeds). | the `asan` and `tsan` presets |
| **Fuzzing** | The decoders of the log, blocks, tables and version edits, the recovery of the table set from `CURRENT` and a MANIFEST, and the whole database under random operations, under libFuzzer with the sanitizers. The write batch decoder is not fuzzed. | `fuzz/` |
| **The crash test** | A real process writing to a real filesystem, killed with `SIGKILL` at a random moment, and the database checked after. | `tools/shale_crashtest.cc` |
| **Tests of the repository** | The scripts that enforce layering, hygiene and links, the CMake options, and the documents' numbers. | `tests/scripts`, `tests/docs` |

## Running them

The presets are in `CMakePresets.json`. All of these need CMake 3.25 and Ninja; GoogleTest and Google
Benchmark are fetched at pinned tags.

```
cmake --preset dev && cmake --build --preset dev         # Debug
ctest --preset dev                                       # everything

./build/dev/tests/log_test                               # one test binary
./build/dev/tests/db_recovery_test --gtest_filter='DBRecoveryTest.TornWalTailIsTruncated'

ctest --preset dev -L stress                             # the stress tests only
```

| What | How |
| --- | --- |
| **Sanitizers** | `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`, and the same with `tsan`. CI runs every test three times under `tsan`: `ctest --preset tsan --repeat until-fail:3 -E '^(sim|determinism)_test$'`, and the simulator once with `SHALE_SIM_SEEDS=20 ctest --preset tsan -R '^(sim|determinism)_test$'` (it is single-threaded, so there is nothing for ThreadSanitizer to find in it and three runs would take most of the job's time). |
| **The simulator, longer** | `SHALE_SIM_SEEDS=500 SHALE_SIM_STEPS=5000 ./build/dev/tests/sim_test --gtest_filter='SimulationTest.RandomSeedsHoldAllInvariants'` |
| **Injected flaws** | `scripts/run-flaws.sh`, or one: `scripts/run-flaws.sh skip_wal_sync`. `--list` prints the names. Each builds a Release tree in `build/flaw-<name>`. |
| **Fuzzing** | Needs a Clang that ships libFuzzer (Apple's does not; Homebrew's `llvm` does). `CC=clang CXX=clang++ cmake --preset fuzz && cmake --build --preset fuzz --target fuzz_targets`, then for a target: `build/fuzz/fuzz/fuzz_table -max_total_time=60 build/fuzz/found/fuzz_table build/fuzz/fuzz/corpus/fuzz_table`. The targets are listed in `fuzz/fuzz_targets.txt`; CI and `preflight.sh` read the same file. To reproduce a crash, give the target the crash file as its only argument. |
| **The crash test** | `cmake -S . -B build/crash -G Ninja -DCMAKE_BUILD_TYPE=Release -DSHALE_BUILD_TESTS=OFF && cmake --build build/crash --target shale_crashtest`, then `build/crash/tools/shale_crashtest --rounds=200 --seed=1 --min_compactions=1 --db=/tmp/crash/db`. The directory must not hold a database. Exit status 0 passed, 1 a check failed, 2 usage, 3 the run could not reach what it is for. |
| **Benchmark smoke** | `-DSHALE_BUILD_BENCH=ON -DSHALE_BUILD_TESTS=OFF`, then `shale_bench --num=20000 --sync_num=200 --repeat=1 --db=DIR`. It asserts completion and that every read matched what was written; it holds no number. The measured figures are in [benchmarks](benchmarks.md). |
| **Coverage** | `cmake --preset coverage && cmake --build --preset coverage && ctest --preset coverage`, then `mkdir -p coverage && gcovr --root . --filter src/ --gcov-executable gcov-14 --fail-under-line 90 --txt-summary --html-details coverage/index.html` (the `gcov` that goes with your GCC). |
| **Everything CI does** | `scripts/preflight.sh`. It takes about half an hour. |

## The simulator

`tests/testkit/simulator.h` is the documentation; this is the summary.

A run opens a database over a `FaultInjectionEnv` (an in-memory filesystem that models the gap between
what a program wrote and what a disk kept) and a `ManualScheduler`, with options small enough that a
few kilobytes of writes fill a memtable and a few of them fill a level. For a given number of steps
it picks an operation by weight: a put, a delete, a multi-operation batch, a get, a scan, a snapshot
taken, read or released, an iterator opened or stepped, a flush, a range compaction, one background
task, a power loss, a process crash, a failure injected into one kind of `Env` call, or read
corruption for the next twenty steps. The default run is **50 seeds of 2000 steps over 200 keys**; the
key space is small so that keys are overwritten and deleted again and again.

After every step the invariants are checked against a reference model (`ModelDB`, a `std::map` and its
copies for snapshots):

| | Checks |
| --- | --- |
| **I1** | Every key reads back as the model says, at a snapshot or not, and a scan returns exactly the model's entries in order: nothing missing, nothing extra, nothing out of place. While read corruption is armed, an OK read must still be right. |
| **I2** | The version is sound: levels in order and disjoint below 0, every live file present with the size the MANIFEST gives, the sequence number never going back (and, after a reopen, not below the durable floor: the last sequence number of the newest synced write), and nothing in the tables that the database has not seen. |
| **I3** | When background work is idle and nothing can explain a leftover, the directory holds no table that no live version names. |
| **I4** | After every crash and reopen, the database holds the state after some **prefix** of the writes, at least as long as the last synced write, and a batch is never half there (D1, D2 and D3 of [ADR 0004](design/0004-durability.md)). Held in full after an open that read corrupted bytes and worked. |
| **I5** | An error is allowed only where an injected fault that fired explains it, and, where only calls were failed, the error must say it was injected. |
| **I6** | A database that has stopped writing goes on refusing writes; the next write after an error must fail. |

**The checkers are tested too.** `ModelDB` and each invariant have cases that must alarm and cases that
must not, using a database wrapper that misbehaves in one named way (`tests/sim/invariants_test.cc`,
`tests/sim/sim_test.cc`: `SimulationDoublesTest`). A run that gave a wrong answer and did not fail would
make the whole simulator worthless, so each wrapper must make a run fail with the right invariant.

**The simulator is held to be fierce.** At the end of the default run it checks that it provoked enough:
the number of power losses, process crashes, flushes, compactions, tombstones dropped, snapshots and
iterators held across a compaction, injected errors and corrupted reads all have floors, and the run fails
under them, as it does if no table ever reached level 2. A simulator that quietly stopped crashing the
database would otherwise pass.

**It is deterministic.** One seed is one run, on every toolchain: `tests/sim/determinism_test.cc` asserts
that a seed run twice has the same trace digest and pins the digests of three seeds, so a toolchain that
diverges fails.

**Its crash model.** A crash is made between two operations, never inside one, and a flush or a compaction
is one operation to the driver. What a crash does to the middle of one is reached the other way: an armed
failure of an `Env` call fires inside the task, and the reopen that follows (after the next write has
been shown to fail) is a power loss in four cases of six. The filesystem it crashes is described at the
top of `tests/testkit/fault_env.h`, including what it does not model.

## Reproducing a seed

A failing run prints the invariant that failed, `seed=<n> step=<k>`, and the last 50 operations. The same
seed fails the same way, on any machine:

```
cmake --preset dev && cmake --build --preset dev --target sim_test
SHALE_SIM_SEED=2240 SHALE_SIM_VERBOSE=1 ./build/dev/tests/sim_test \
  --gtest_filter='SimulationTest.RandomSeedsHoldAllInvariants'
```

The variables:

| Variable | Meaning |
| --- | --- |
| `SHALE_SIM_SEED` | Run that one seed and nothing else. |
| `SHALE_SIM_SEEDS` | How many seeds the main test runs, from 0 (default 50). |
| `SHALE_SIM_STEPS` | Steps per seed (default 2000). |
| `SHALE_SIM_KEYS` | Different keys per seed (default 200). |
| `SHALE_SIM_VERBOSE` | Print each seed before it runs, which names the seed of a run that aborts, and its trace digest after. |

Use the same `STEPS` and `KEYS` as the run that failed; a seed is a function of all three. To look inside
a run, run it under a debugger or add `Trace` calls: nothing in the run depends on time, threads or the
machine, so the run you look at is the run that failed.

## Adding a regression seed

A seed that once failed is kept, so that the bug cannot come back unseen.

1. Reproduce it as above, and fix the engine.
2. Run the seed again; it must pass.
3. Add a line to `tests/sim/regression_seeds.txt`: the seed, then a comment that says what it caught.

   ```
   2240  # the failed switch to a MANIFEST deleted the file CURRENT names, after a misread of CURRENT
   ```

   Lines that start with `#` and the text after the number are comments. `SimulationTest.RegressionSeeds`
   runs each of them with the default steps and keys in every build.
4. Where the bug can be pinned without the simulator, **also write the unit test that fails before the
   fix**, and say so in the commit. A seed is a long way from the line that was wrong, and it stops
   being a reproduction the day the simulator's weights change; a unit test does not. The commits that
   fixed the seeds below did both.
5. Name the seed in the commit message.

A seed in the file is not a proof that the bug is fixed *everywhere*: the simulator found it by chance
among 2000 steps, and a different seed may reach the same defect by a different path. That is why the
default run covers many seeds and why `SHALE_SIM_SEEDS` is there to be turned up.

## Injected flaws

A test suite that has only ever passed has not shown that it can fail. The engine carries six
deliberate flaws, each a `#if defined(SHALE_FLAW_<NAME>)` branch in the source that only
`-DSHALE_FLAW=<name>` compiles in. The `flaws` CI job builds the simulator with each one in turn, and
**the job passes only if the simulator fails with an invariant's id**. A run that fails any other way,
or builds but passes, is a red job.

| Flaw | Where | What it breaks | First reported by |
| --- | --- | --- | --- |
| `skip_wal_sync` | `src/db/db_write.cc` (the sync of a write, and of the old log by the first synced write after a memtable switch) | an acknowledged synced write is not durable | I2 (I4 on some seeds) |
| `skip_dir_sync` | `src/db/filename_io.cc` (`SyncDirectory`) | a new file's directory entry is not durable | I2 |
| `skip_manifest_sync` | `src/db/version_set.cc` (`LogAndApply`) | an edit is installed before it is durable | I2 (I4 on some seeds) |
| `drop_tombstones_early` | `src/core/compaction_filter.cc` | a deletion is dropped although a deeper level may hold the key, so the old value comes back | I1 |
| `ignore_snapshots_in_compaction` | `src/core/compaction_filter.cc` | a compaction forgets the open snapshots and drops what they see | I1 |
| `delete_live_files` | `src/db/db_background.cc` (`DeleteObsoleteFiles`) | a table that an older version still reads is deleted | I2 |

The last column is the invariant that reported each flaw first, at seed 0, when the job was added; the
job prints the one that catches it in its summary, and each flaw fails the first twenty seeds when
they are replayed one by one. The simulator needed no change to catch them.

To add a flaw: add its name to `SHALE_FLAW_NAMES` in `cmake/ShaleOptions.cmake`, add the `#if` branch
(a flaw must be a different build and never a runtime option, so that the shipped code cannot contain
it), and add the name to the matrix of the `flaws` job. `tests/scripts/flaws_test.sh` fails if the two
lists disagree.

## Stress

`tests/stress/concurrency_test.cc` runs real threads against an open database with its own scheduler
thread, over an in-memory `Env`, with a write buffer and levels of the smallest size that is allowed so
that a few thousand writes make dozens of flushes and compactions while the threads read:

- `ReadYourWritesAndMonotonicVersions`: a write that returned is visible to its writer, and the versions
  of a key that one writer sees never go back.
- `IteratorsSeeAtomicBatchesDuringCompaction`: scans that run across flushes and compactions see a batch
  whole or not at all.
- `SnapshotReadsAreStable`: a snapshot's reads do not change as writes, flushes and compactions go on.

Each test also checks that it measured what it claims: that the flush and compaction counters moved,
that readers read while a writer was part of the way through. No test waits for a fixed time; every
wait is a bounded poll. CI runs them (with everything else) three times under ThreadSanitizer, because a
race is a thing that does not happen on every run.

## Fuzzing

Six libFuzzer targets in `fuzz/`, built with AddressSanitizer and UndefinedBehaviorSanitizer:

| Target | Feeds garbage to |
| --- | --- |
| `fuzz_log_reader` | the log reader, with oracles for what it must and must not report |
| `fuzz_block` | the block decoder |
| `fuzz_table` | `Table::Open`, `Get` and iteration, with oracles for the answers |
| `fuzz_version_edit` | the version edit decoder, with a round-trip oracle: an edit that decodes must encode back to what it was |
| `fuzz_version_set` | the recovery of the table set from a fuzzed `CURRENT` and MANIFEST in memory |
| `fuzz_db_ops` | the whole database, with the input read as a program of operations, compared with `ModelDB` |

The seed corpora are made by `fuzz/make_corpus.cc`. CI runs each target for **60 seconds**: a smoke run,
long enough to start from the corpus and to catch what a change has just made easy to reach. It is
not a fuzzing campaign, and a clean 60 seconds says nothing about inputs it did not try. A crash uploads
the input that caused it as an artifact.

## The real crash test

`shale_crashtest` forks a writer that applies numbered puts and deletes to a small set of keys, so that
every memtable that is flushed spans the whole set, and reports each operation to the parent when the
call returns OK. After a random delay the parent kills the writer with `SIGKILL`, reopens the database
and checks it against a per-key model: each key holds what the last *acknowledged* operation on it left,
the one operation in flight at the kill may have landed or not (on its own key only), and nothing else
is there. It also watches the directory for tables that appear and are removed, so a run shows whether
the kills reached flushes and compactions at all, and `--min_compactions=N` fails a run that never did.

`--self_test_damage` overwrites one key with a wrong value before the check and the run must fail,
naming the key (`tests/tools/crashtest_damage_test.sh` runs it): the check is shown to be able to fail.

CI kills the writer 200 times on Linux and macOS (about a minute), and a further six rounds with
`--sync`. A process kill leaves the whole page cache behind, so **this test says nothing about power
loss**; that is the simulator's job. What it shows is that a kill at any point of a write, a flush or
a compaction leaves a database that opens and holds everything it said it did, on a real filesystem.

## Tests of the repository itself

- `tests/scripts/layering_test.sh` and `check-layering.sh`: which headers each layer may include; that
  nothing under `src/` or `include/` writes to standard output or error; that nothing under `src/` or
  `include/` throws; and that no C++ under `include/`, `src/`, `tests/` or `tools/` uses `std::rand` or
  `std::random_device` or includes `<random>`.
- `tests/scripts/hygiene_test.sh` and `check-hygiene.sh`: no Chinese, Japanese or Korean characters, and
  none of the authorship credits that `scripts/attribution-patterns.txt` lists (a co-author trailer, a
  line that says what produced a change, the robot emoji), in any tracked file or commit message. Each
  pattern has a sample that only it catches.
- `tests/scripts/links_test.sh` and `check-links.sh`: every relative link in every Markdown file resolves,
  and every `#fragment` names a heading.
- `tests/scripts/options_test.sh`: the CMake options accept what they should and refuse the rest, and the
  installed package's version rule.
- `tests/scripts/flaws_test.sh`: the CI matrix of flaws and the list in CMake agree.
- `tests/scripts/release_check_test.sh` and `release-check.sh`: a tag is accepted only when it is `v` and
  the version of `CMakeLists.txt`, and a refusal prints both; the notes of a release are exactly its
  section of `CHANGELOG.md`; and the version of this tree and its changelog entry agree.
- `tests/scripts/release_workflow_test.sh`: the release workflow runs on a tag and nowhere else, only its
  publishing job can write, and it, [RELEASING.md](../RELEASING.md) and the script name the same files.
- `tests/docs/format_constants_test.cc`: [format](format.md) says what the code does; its tables of
  constants, layouts, file names and `CURRENT` messages and its worked examples are read and compared
  with the code, and the Bloom filter, the checksum mask and the hash are recomputed from the rules it
  states.
- `tests/docs/operations_doc_test.cc`: the defaults, limits, properties and counters in
  [operations](operations.md) are what the code has, and an option or a property that is added to
  `options.h` or `db.h` and not to the guide fails the test.
- `tests/docs/readme_test.cc` and `tests/tools/readme_transcript_test.sh`: the README's program is
  `examples/quickstart.cc`, and its "Try it" transcript is what its commands print when they are run
  again.
- `tests/compile_fail/`: Clang's thread-safety analysis rejects an unguarded access (the build is tested
  because the analysis could silently stop working).

## CI and preflight

Every job has a time limit. The jobs that can fail on a seed, a database or an input upload it when they
do: `build-test`, `sanitize` and `clean-clone` their test log, `flaws` the simulator's output, `crash` the
database, `fuzz` the input; `coverage` and `bench-smoke` upload their reports. `lint` uploads nothing.

| Job | What it runs |
| --- | --- |
| `build-test` | Ubuntu 24.04 with GCC 13, GCC 14 and Clang 18, Debug and Release, and Clang 17, Release; macOS 14 with Apple Clang, Release. Configure, build, `ctest`. |
| `sanitize` | Clang 18, AddressSanitizer with UndefinedBehaviorSanitizer; and ThreadSanitizer, each test three times, the simulator once. |
| `flaws` | One job for each injected flaw; the simulator must fail. |
| `crash` | `shale_crashtest`, 200 rounds and six synced rounds, on Ubuntu and macOS. |
| `fuzz` | Every target for 60 seconds. |
| `lint` | `clang-format`, the layering, hygiene and links checks, `clang-tidy` with warnings as errors on `src/` and `tools/`, and Doxygen over the public headers with warnings as errors. |
| `bench-smoke` | `shale_bench` on a small database, and the micro benchmarks for a moment. No number is held. |
| `coverage` | GCC 14 with `--coverage` and gcovr; the report is uploaded; the job **fails under 90% of the lines of `src/`**. |
| `clean-clone` | A fresh checkout with no cache: configure, build and test the Release preset; install; build and run `examples/` against the installation with `find_package(shale)`; check that a request for another major version is refused; write and read a key with `shalectl`. |

`scripts/preflight.sh` runs the same things locally, in CI's order, and fails if a tool it needs is
missing or has the wrong version.

**Coverage**, measured by CI on Linux with GCC 14: **95.4% of lines, 95.2% of functions, 64.8% of
branches** of `src/`. The same build on macOS with GCC 16 measures 92.3% of lines. The floor of 90% is an
alarm for a measurement that is empty or broken and for a large part of `src/` that the tests no longer
run; it is not a target, and nobody should write a test to move the number. Branch coverage is reported
and held to no floor: gcov counts the branches the compiler generates (in inlined standard-library code,
for example) as well as the ones written in `src/`, so it reads lower than the line figure.

## What the tests do not cover

Two kinds of gap: what a developer's machine cannot run and CI does, and what **nothing** here runs.

### What local runs cannot cover, and CI covers

The author's machine is a Mac (arm64) with Apple Clang, Homebrew Clang and GCC 16.

- **Linux behaviour.** The test of a full disk, which writes to `/dev/full` and expects
  `No space left on device` (`PosixEnvTest.WriteFailureNamesThePathAndTheCause`, which exists on Linux
  only), the `fdatasync` path, and the real crash test on **ext4** (the GitHub-hosted runners'
  filesystem). Locally the engine runs on APFS with `F_FULLFSYNC`, which CI's macOS jobs also cover.
- **GCC 13 and 14, and Clang 17 and 18.** The compiler floors are GCC 13, Clang 17 and Apple Clang 15;
  the matrix builds each floor, and GCC 14 and Clang 18 above them (Clang 17 in Release only).
- **The coverage figure** that CI reports (95.4% of lines) is a GCC 14 on Linux number; the local build
  measures 92.3% with GCC 16 on macOS.
- **A cold clone.** The `clean-clone` job is the only run that starts from nothing.

### What nothing covers

- **A real power loss.** The simulator models "bytes that were not synced are lost". It does not model a
  device that acknowledges a flush it has not done, so a drive that lies breaks D1 and D2 and nothing
  here can tell. A process kill leaves the page cache, so the crash test is not a power cut either.
- **How a controller or a filesystem reorders writes**, beyond what the simulator's model of ordered
  directory changes and prefix-only file loss says. Anything the POSIX interface does not promise is
  outside what the tests can reach.
- **What the simulator's filesystem leaves out** (the list is at the top of `tests/testkit/fault_env.h`):
  a power loss leaves a clean prefix of a file, never zero-filled or garbage tails; `CreateDir` and
  `RemoveDir` are durable at once; a failed call has no partial effect; stored bytes never change by
  themselves (a read can come back with a flipped bit, but the bytes at rest do not rot).
- **Runs longer or bigger than CI's.** The simulator runs 50 seeds of 2000 steps over 200 keys, the crash
  test 200 rounds, each fuzz target 60 seconds, and the stress tests a few thousand writes. A database of
  tens of gigabytes, a year of uptime and a compaction that takes an hour have never been run.
- **Other filesystems.** Only ext4 (CI) and APFS (macOS) have been used. XFS, btrfs, ZFS, NFS and FUSE
  filesystems have not.
- **Other platforms.** x86-64 Linux and arm64 macOS only: no 32-bit target and no big-endian machine,
  although every integer in a file is written little-endian explicitly. Windows is not supported.
- **Thread interleavings in the simulator.** It is single-threaded; races are looked for by the stress
  tests and ThreadSanitizer, which sample interleavings and do not enumerate them.
- **The fuzzers are a smoke run** in CI, not a campaign.
- **Performance regressions.** The benchmark job holds no number, because a shared runner is too noisy
  for any to be held to. The figures in [benchmarks](benchmarks.md) were taken once, on one machine.

If you find a way to break the engine that these tests could not have shown, that is a gap in this list:
see [CONTRIBUTING](../CONTRIBUTING.md) for how to report it and how to turn it into a seed or a test.
