# Contributing to shale

Thank you for looking. shale is small on purpose and is held to a high standard of proof, so a change
is welcome when it comes with the test that shows it is right. This page is the shortest route to a
change that gets merged.

By taking part you agree to the [Code of Conduct](CODE_OF_CONDUCT.md). To report a security problem,
read [SECURITY.md](SECURITY.md) first and do not open a public issue.

## Before you start

- **Read** the [README](README.md), [docs/architecture.md](docs/architecture.md) and
  [docs/testing.md](docs/testing.md). The design records in [docs/design](docs/design/) say why the big
  decisions are what they are, and [0007](docs/design/0007-scope.md) lists what is deliberately not built.
  A change that adds something on that list needs a conversation first: open an issue.
- **Open an issue** for anything bigger than a typo, so that the shape of the change can be agreed before
  the work is done.
- A bug report is best with a **seed**. If the simulator found it, give the seed, the number of steps and
  keys if they are not the defaults, and the toolchain; see
  [reproducing a seed](docs/testing.md#reproducing-a-seed). If something else found it, give the smallest
  program or the directory that shows it.

## Building and testing

You need a C++20 compiler (GCC 13, Clang 17 or Apple Clang 15 or newer), CMake 3.25 and Ninja. Nothing
else is needed at runtime; GoogleTest and Google Benchmark are fetched at pinned tags for the tests and
the benchmarks.

```
cmake --preset dev && cmake --build --preset dev
ctest --preset dev
```

The other presets are `release`, `asan`, `tsan`, `coverage` and `fuzz`. [docs/testing.md](docs/testing.md)
says what each layer of tests is for and how to run it. Before you push, run **`scripts/preflight.sh`**: it
does everything CI does, in CI's order, and fails if a tool it needs is missing. It takes about half an
hour, which is cheaper than a red build. At the least, run `ctest --preset dev`,
`scripts/check-layering.sh`, `scripts/check-hygiene.sh` and `scripts/check-links.sh`.

## The rules

These are checked, by the compiler, by a script, or by a test, and a change that breaks one is refused by
CI whatever else it does.

- **C++20, no extensions.** No third-party code at runtime.
- **Warnings are errors**: `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Werror`, and `-Wthread-safety` under Clang, on every target
  including tests, tools and fuzzers.
- **Errors are values.** A function that can fail returns a `shale::Status` (it is `[[nodiscard]]`).
  **Nothing under `src/` throws.** A caller's mistake is `InvalidArgument`; `SHALE_CHECK` is for an internal
  invariant that has been broken, and aborts.
- **Layering** ([architecture](docs/architecture.md#layers)): `src/core/` includes no thread, mutex, atomic,
  clock, file or random header; `src/util/` includes only `util/`, `shale/` and the standard library; and
  so on. `scripts/check-layering.sh` reads every `#include`.
- **Nothing under `src/` or `include/` writes to standard output or error**, except the one line that
  `src/util/check.cc` prints before it aborts. Tools under `tools/` may print.
- **No mutable static or global state.** `Env::Default()` returns a function-local static that holds none.
- **All randomness is `shale::Random` with an explicit seed.** `std::rand`, `std::random_device` and the
  `<random>` distributions are banned in `src/`, `tests/` and `tools/`, because a distribution is
  implementation-defined and a seed would not reproduce across standard libraries.
- **Every public item in `include/shale/` has a `///` comment.** Doxygen runs with warnings as errors.
- **Formatting** is `clang-format` with the repository's `.clang-format` (Google style, 100 columns), at
  the version CI pins, and **`clang-tidy`** with the repository's `.clang-tidy` and warnings as errors, on
  `src/` and `tools/`.
- **Everything in the repository is English**: code, comments, documents, commit messages, test names.
  `scripts/check-hygiene.sh` checks that in every tracked file and every commit message.

## Tests come first

1. **Write the test that fails**, run it, and see it fail for the reason you expect.
2. Change the code until it passes.
3. Keep the test.

A fix with no test that fails without it is not finished. Put the test in the layer that can catch the
bug most cheaply: a unit test for a decoder or a function, the simulator for something that needs a crash
or a fault, a stress test for a race. A decoder gets three kinds of test: truncated input, random bit
flips, and length fields past the end, each of which must give `Corruption` and never a crash.

If the simulator found the bug, add its seed to `tests/sim/regression_seeds.txt` **and**, where it can be
pinned without the simulator, a unit test, as in [adding a regression seed](docs/testing.md#adding-a-regression-seed).

A change to the disk format changes [docs/format.md](docs/format.md) in the same commit: its tables and
worked examples are read by `tests/docs/format_constants_test.cc`, which fails if they disagree with the
code. The same goes for a default or a limit in [docs/operations.md](docs/operations.md).

## Commits

[Conventional Commits](https://www.conventionalcommits.org/), with the **module as the scope**:
`feat(db): ...`, `fix(io): ...`, `test(sim): ...`, `docs: ...`. The scopes are `util`, `io`, `core`, `db`,
`sim`, `tools`, `fuzz`, `bench`, `build`, `ci` and `docs`.

The subject says what changed. The body is where the reasoning goes, and is not optional for anything but
a trivial change:

- **Why**: what was wrong or missing, and how you know.
- **What**: what the change does.
- **Considered and rejected**: the alternatives and why not.
- **Not done**: what this leaves for later, honestly.
- **Verified**: what you ran, and what it showed, including the test that failed before the change.

A commit is one logical change that builds and passes its own tests.

## Pull requests

- Branch as `<type>/<issue>-<slug>`, for example `fix/42-torn-manifest`.
- Keep it small enough to review. Say what you ran.
- CI must be green. It runs the compiler matrix, the sanitizers, the injected flaws, the crash test, the
  fuzzers, the lint checks, coverage and a clean-clone build; see
  [CI and preflight](docs/testing.md#ci-and-preflight).
- Pull requests are squash-merged, so the title and description become the commit; write them like one.

## What gets a change refused

- It weakens a check to make something pass: a looser oracle, a test that cannot fail, a floor lowered.
- It adds a dependency, a global, a throw, or output from library code.
- It claims something the tests do not show. The documents are held to the same rule as the code: if a
  sentence says the engine does something, there is a test, a commit or a measurement behind it.
