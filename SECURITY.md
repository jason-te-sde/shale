# Security policy

## Supported versions

Only the latest release of shale receives fixes. shale is at version 0.1 and has no long-term support
line.

| Version | Supported |
| --- | --- |
| 0.1.x | yes |

## Reporting a vulnerability

**Please do not open a public issue for a security problem.**

Report it privately, by email to the maintainer at te.r@northeastern.edu, with `shale security` in the
subject. The repository does not use GitHub's private vulnerability reporting.

Please include:

- what you found and what it lets an attacker do;
- the version or commit, the compiler and the operating system;
- the smallest input that shows it: a database directory, a single file (a table, a log, a MANIFEST) or a
  program. A fuzzer's crash file is ideal: `fuzz/` reproduces it when the file is the only argument.

I aim to acknowledge a report within a week, to say whether I accept it as a vulnerability within two
weeks, and to publish a fix and an advisory together. This is a one-person project, and those are goals,
not guarantees.

## What counts

shale is an embedded library with no network surface, so the threat is a **crafted or damaged database
directory**, or a crafted call, handed to a process that links the library. These are in scope:

- a crash, an out-of-bounds read or write, a use after free, an integer overflow that leads to either,
  or any other undefined behaviour, on the bytes of a table, a write-ahead log, a MANIFEST or `CURRENT`.
  The engine's rule is that everything read from disk is untrusted: a decoder stays inside its buffer,
  and a read from a file that was damaged returns what was written or a `Corruption`. A file that
  breaks the first is a bug, and a security bug if it can be reached by someone who does not already
  control the process;
- a wrong value returned by a read from a table that the engine wrote and that was damaged afterwards
  (a bit flip, a truncation), with `verify_checksums` on, the default. A file made by something that
  computes the checksums for whatever it writes is promised only that no read leaves its buffers
  ([docs/format.md](docs/format.md#what-a-reader-checks)), so a wrong value from a lookup in one is a known limit and
  not a vulnerability;
- an input that makes the engine allocate memory without bound relative to the size of the file or call;
- a way around the limits documented in [docs/design/0007-scope.md](docs/design/0007-scope.md) and
  `include/shale/options.h` (key and value sizes, option ranges).

These are **out of scope**:

- anything that needs write access to the database directory *and* intends only what write access already
  allows (deleting files, filling the disk, replacing the database with another). shale does not
  authenticate or encrypt: the permissions on the directory are the access control, and a database file
  is not a place to keep secrets.
- a device that loses data it said was durable. [docs/design/0004-durability.md](docs/design/0004-durability.md)
  says what the durability promises assume.
- the loss of a write that was acknowledged with `WriteOptions::sync = false` in a power cut, which is
  the documented meaning of the setting.
- a denial of service by a legitimate workload: a large database takes a long time to compact.
- a flaw in a dependency used only for tests (GoogleTest, Google Benchmark): report it upstream.

## What happens to a fix

A confirmed vulnerability is fixed on a private branch, released as a patch version (see
[RELEASING.md](RELEASING.md)), and described in a GitHub security advisory and in
[CHANGELOG.md](CHANGELOG.md) once users have a fixed version. The report is credited unless you ask not
to be.
