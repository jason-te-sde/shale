# 0005. The table format

Status: accepted

## Context

A table is the engine's one durable, immutable, sorted structure, and everything that is read from a
disk is untrusted until a check says otherwise. The format has to let a point lookup read one block, let
a scan walk forward cheaply, say with a checksum whether any byte of a block is wrong, and keep the
whole of a reader's arguments about hostile input short.

## Decision

The layout is LevelDB's with changes. The bytes are in [format](../format.md#tables); what was chosen
and why:

```
[data blocks, each with a trailer] [filter block + trailer] [index block + trailer] [48-byte footer]
```

- **Blocks with shared-prefix keys and restart points.** An entry stores only the bytes of its key that
  differ from the previous one's, and every 16th entry stores its whole key and goes in a restart array.
  A seek binary-searches the restart keys, then walks. 16 and the 4 KiB block size are LevelDB's
  defaults; both are options.
- **A trailer on every block**: a type byte (always 0 today) and a masked CRC-32C of the contents and
  the type byte, verified on every read (`ReadOptions::verify_checksums` is on by default). The type byte
  is the place a compression type would go.
- **One Bloom filter for the whole table, over user keys.** It is built from every distinct user key,
  stored as one block, and loaded when the table is opened. A lookup asks it before it touches the index
  or a data block. It is over *user* keys because a lookup asks about one, and one entry has to answer
  for every version of it.
- **The index holds the last internal key of each data block**, unshortened, and a block handle. It is
  a block of the same layout with a restart point at every entry.
- **A 48-byte footer** with the two handles that are not discoverable otherwise (filter, index), a
  format version, its own CRC-32C (not masked, since it follows the bytes it covers), and a magic number
  whose last byte is the last byte of the file. A reader checks the size, the magic, the checksum and
  the version, in that order.
- **No metaindex block.** The footer names the filter and the index directly. There is only one of
  each, so nothing needs a table of contents.
- **Hostile input is the default assumption.** `Table::Open` checks that the index is sorted and that its
  handles name different, ordered blocks before the filter; the block iterator refuses a key shorter than
  the 8-byte tag before the comparator sees it (which would abort); the table iterator ends with
  `Corruption` at a key out of order or after the last key its index entry names; and a block is put in
  the shared cache only after its checksum was checked.

## Consequences

**What this buys.** A point lookup on a table reads at most one data block, and none when the filter says
no. Any single flipped byte in a block is a `Corruption`, never a wrong answer. A reader's contract is
small: a table either yields the entries that were written, in order, or an error.

**What it costs.**

- **The whole filter and the whole index are in memory for every open table**, whatever the table's size,
  and the filter's size is proportional to the number of keys (10 bits per key by default). With
  `max_open_files` tables open, that is the memory floor of the table cache; the block cache is on top.
- **Building a filter holds one 32-bit hash for every distinct key of the table** until `Finish()`.
  A table is about `target_file_size` when a compaction wrote it and about `write_buffer_size` when a
  flush did, so the cost is bounded, but it is not free.
- **The index is larger than it could be.** An index key is a whole internal key, not the shortest key
  that separates two blocks, so the index of a table with long keys is bigger.
- **No compression.** A table takes the bytes of its keys and values, less prefix compression of keys. A
  value that compresses well takes more disk than it needs to. The type byte reserves the place.
- **A filter is a probabilistic answer.** With 100,000 keys of four binary bytes and 10 bits per key it lets
  1.5% of absent keys through (0.26% at 15 bits; commit `adb4509`), and with text keys of ten or so bytes
  about half as much; `BloomTest.FalsePositiveRatesAtTenAndFifteenBitsPerKey` in `tests/io/bloom_test.cc`
  measures and bounds all four. Each key that gets through costs a data block read.
- **There is no prefix or range filter.** A scan cannot skip a table by its range of prefixes.
- **One filter per table, not per block.** A table's filter cannot be partially loaded; a very large table
  would load a very large filter. Tables here are small by construction (2 MiB targets).

## Alternatives considered

- **LevelDB's per-block filter** (a filter for each 2 KiB of data, found through a metaindex block).
  Rejected: more moving parts for the same effect at this size, and a metaindex block that would have
  exactly one kind of content.
- **Shortened index keys** (LevelDB's `FindShortestSeparator`). Rejected as an optimisation that is not
  needed for correctness: the last key of a block is a valid separator. See [ADR 0007](0007-scope.md).
- **Compression.** Rejected as a dependency: Snappy and Zstd would be the engine's first third-party
  runtime code. The format keeps the type byte, so adding it changes values and not the layout.
- **A masked checksum in the footer.** Rejected: masking exists for a CRC that is stored among the bytes
  it covers, and the footer's follows them.
- **Partitioned index and filter blocks.** Rejected: they matter when an index or a filter no longer fits
  in memory, which a 2 MiB table does not reach.
- **Memory-mapped reads.** Rejected: a file that is truncated, or whose disk fails, under a mapping
  raises a signal instead of returning a `Status`, which a library cannot recover from, and a read
  would hand out bytes that were never passed through a check. See [ADR 0007](0007-scope.md).
- **A different checksum (CRC-64, xxHash).** Rejected: CRC-32C is the one that LevelDB's formats use and
  that CPUs have instructions for (the engine uses them on ARM builds that enable the CRC extension,
  and a table-driven version everywhere else), and the checksum protects blocks of a few kilobytes,
  for which 32 bits is enough.

## References

- `src/io/table_format.h`, `block_builder.h`, `block.h`, `bloom.h`, `table_builder.h`, `table.h`
- Tests: `tests/io/table_format_test.cc`, `table_builder_test.cc`, `block_test.cc`, `bloom_test.cc`,
  `table_test.cc`; `fuzz/fuzz_block.cc`, `fuzz/fuzz_table.cc`; and
  `tests/docs/format_constants_test.cc`, which holds [format](../format.md) to the code.
- Commits `216e7fc` (an unverified block must not enter the shared cache), `07376bb` (refuse short
  block keys before the comparator sees them).
