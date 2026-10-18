# On-disk format

This is every byte shale writes to a database directory. It is meant to be enough to write a reader
for any of the files without looking at the source.

Nothing here is a promise by prose alone. The table of constants, the table of fixed layouts, the
table of file names and the worked examples are read by
[`tests/docs/format_constants_test.cc`](../tests/docs/format_constants_test.cc), which compares each
of them with the code that writes the file and feeds the examples to the code that reads it. A number in
one of those tables, or a byte of one of those examples, that disagrees with the code fails that test.
A number that appears only in the prose around them is not read by the test; each one that matters is
also in a table. The `<!-- ... -->` lines that you do not see when this is rendered are what the test
finds its tables and examples by.

## Contents

- [Conventions](#conventions)
- [The files of a database](#the-files-of-a-database)
- [Internal keys](#internal-keys)
- [Write batches](#write-batches)
- [The write-ahead log](#the-write-ahead-log)
- [Tables](#tables)
- [The MANIFEST and version edits](#the-manifest-and-version-edits)
- [CURRENT](#current)
- [The memtable](#the-memtable)
- [Compatibility](#compatibility)
- [Fixed layouts](#fixed-layouts)
- [Constants](#constants)

## Conventions

- Every integer is **little-endian**, whatever machine wrote it. `fixed32` and `fixed64` are 4 and 8
  bytes. A field written `fixed16` is 2 bytes.
- A **varint** holds 7 bits of the value in each byte, low bits first, and sets the high bit of every
  byte but the last. A varint32 is 1 to 5 bytes and a varint64 is 1 to 10.
- A **length-prefixed string** is a varint32 length followed by that many bytes.
- Every checksum is **CRC-32C** (Castagnoli). Where a checksum is stored inside bytes that it
  covers or next to them, it is **masked**, which is a rotation and an addition:

  ```
  masked = ((crc >> 15) | (crc << 17)) + 0xa282ead8     (32-bit arithmetic, wrapping)
  ```

  Masking exists because the CRC of a string that contains its own CRC comes out as a fixed value,
  which makes the check useless. The footer of a table is the one stored checksum that is not
  masked, because it follows the bytes it covers and does not sit among them.
- Keys and values are byte strings. Keys compare as unsigned bytes, a key before any longer key that
  it is a prefix of. There is no custom comparator.

## The files of a database

A database is a directory. Every file the engine creates has a name that is built in one place
(`src/util/filename.cc`) and recognised in the same place, so that recovery can tell its own files
from anything else in the directory, and leave those alone.

<!-- table:files -->
| Kind | File name | Number |
| --- | --- | --- |
| log | `000123.log` | 123 |
| table | `000123.sst` | 123 |
| table | `1234567.sst` | 1234567 |
| manifest | `MANIFEST-000123` | 123 |
| current | `CURRENT` | - |
| lock | `LOCK` | - |
| info_log | `LOG` | - |
| old_info_log | `LOG.old` | - |
| temp | `000123.dbtmp` | 123 |
<!-- /table:files -->

A file number is printed in decimal, padded with zeros to six digits; a larger number is printed in
full, as the second table row shows. **Only that spelling is a name.** `9.log`, `0009.log` and
`MANIFEST-9` are not database files, and recovery neither reads nor deletes them.

| File | What it holds |
| --- | --- |
| `NNNNNN.log` | A write-ahead log: the writes that the tables do not hold yet, one [batch](#write-batches) per record. |
| `NNNNNN.sst` | A sorted table. Immutable once written. |
| `MANIFEST-NNNNNN` | The list of live tables, as a log of [version edits](#the-manifest-and-version-edits). |
| `CURRENT` | The name of the live MANIFEST and a checksum of that name. |
| `LOCK` | Locked with `flock` while a process has the database open. Empty. |
| `LOG`, `LOG.old` | A text log of what the database did, for people. Not read by the engine. `LOG` of the previous run is renamed to `LOG.old` when the database opens. |
| `NNNNNN.dbtmp` | A file being written that has not been renamed into place. `CURRENT` is replaced through one, which takes the number of the MANIFEST it names. |
| `NNNNNN.log.dropped` | A log that a lenient open (`paranoid_checks = false`) set aside. The engine never reads or deletes it again; `DestroyDB()` does. |

File numbers come from one counter, so a number is used once for a log, a table or a MANIFEST and
never again. The counter is bounded: a database refuses a MANIFEST numbered at or above 2^56, and
refuses to write one whose counter is above it (`kMaxFileNumber`, commit `0b3e4b9`).

## Internal keys

Every entry in the write-ahead log, the memtable and the tables carries an **internal key**: the user
key followed by an 8-byte tag.

```
internal key = user key ‖ fixed64( sequence << 8 | type )
```

The sequence number is the position of the write in the database's history, at most 2^56 - 1. The type is
`1` for a value and `0` for a deletion (a tombstone). Internal keys sort by user key ascending, then
by tag **descending**, so that for one user key the newest entry comes first and, at one sequence
number, a value comes before a deletion. A lookup key is built with the type of a value, so that a
seek lands on the newest entry at or below the sequence number it was given.

<!-- example:internal_key -->
```text
6b                        # the user key "k"
01 07 00 00 00 00 00 00   # the tag: (7 << 8) | 1 as a fixed64: sequence 7, a value
```

## Write batches

A write batch is the unit of atomicity, and the write-ahead log stores exactly these bytes, one batch
to a record. A batch is a 12-byte header (the `write_batch_header` rows of the
[fixed layouts](#fixed-layouts): the sequence number of its first operation and the number of
operations) and then the operations.

The operations follow the header, one after another. An operation is a tag byte, which is `1` for a
put and `0` for a deletion, then the key as a length-prefixed string, and for a put the value as a
length-prefixed string.

<!-- example:write_batch -->
```text
07 00 00 00 00 00 00 00                  # sequence number of the first operation: 7
02 00 00 00                              # two operations
01 03 6b 65 79 05 76 61 6c 75 65         # put: tag 1, key "key" (length 3), value "value" (length 5)
00 03 6f 6c 64                           # delete: tag 0, key "old" (length 3)
```

The operation at position *i* has sequence number `sequence + i`. A group of batches that were
written together (see [architecture](architecture.md#the-write-path)) is logged as one record whose
batch is the group's batches joined: the count is the sum of the counts, and the sequence number is
the first batch's.

A reader of a batch that finds the count disagreeing with its records, a record that ends early or
a tag that is neither 0 nor 1 reports `Corruption`. A recovered batch is applied to the memtable
whole or not at all.

## The write-ahead log

The log is a sequence of **blocks** of 32768 bytes. Only the last may be shorter. A block holds
**physical records** back to back, and a physical record never crosses a block boundary.

```
record = checksum (4) ‖ length (2) ‖ type (1) ‖ payload (length bytes)
```

The first three fields are the `wal_record_header` rows of the [fixed layouts](#fixed-layouts).

The checksum is the masked CRC-32C of the type byte followed by the payload. The type is one of:

| Type | Value | Meaning |
| --- | --- | --- |
| zero | 0 | Never written as a record. The zero padding at the end of a block is skipped, but a tail of zeros where a record should begin reads as this, and is reported as a defect. |
| `FULL` | 1 | A whole logical record. |
| `FIRST` | 2 | The first fragment of a record that spans blocks. |
| `MIDDLE` | 3 | A fragment that is neither first nor last. |
| `LAST` | 4 | The last fragment. |

A logical record that fits in what is left of a block is one `FULL` fragment. One that does not is cut
into a `FIRST` fragment that fills the block, any number of `MIDDLE` fragments that fill whole
blocks, and a `LAST`. If fewer than 7 bytes are left in a block, no header fits and they are zero
padding; the next record starts the next block. If exactly 7 are left, the writer fills them with a
`FIRST` or `FULL` fragment of length zero, and the record's payload starts in the next block. An
empty record is a `FULL` fragment of length zero.

The reader finds its way in a damaged file by dropping the rest of a block when it meets a defect and
starting again at the next block, which is why a block boundary is a place where it can always
resynchronise.

<!-- example:wal_record -->
```text
10 f8 3c 51               # checksum: masked CRC-32C of "01" and the payload below
1c 00                     # length: 28
01                        # type: FULL
07 00 00 00 00 00 00 00   # the payload is the batch of the example above
02 00 00 00
01 03 6b 65 79 05 76 61 6c 75 65
00 03 6f 6c 64
```

What the reader reports, and what it does not, is the difference between a crash and corruption, and
it is exact:

- **Silent.** The file stops inside a header, inside a record whose length says it would have fit in
  its block, or after a `FIRST` or `MIDDLE` fragment with no `LAST` after it. That is what a writer
  that died leaves behind, and the records before it are all returned. A header whose length runs past
  the end of the file counts as this only if the bytes that are there are *not* a whole fragment: where
  some length, up to what is left, gives the checksum the header holds, the fragment was whole and its
  length is damaged, and that is reported (commit `89915a1`).
- **Reported**, once per defect, with the bytes it drops: a read error; `checksum mismatch`; `bad
  record length`; `unknown record type` (zero included); `missing start of fragmented record`; and
  `partial record without end`. A defect drops the rest of its block.

A report is never "the log ends here", wherever in the file it falls, because dropping the rest of a
block can drop intact records that were acknowledged. What `Open` does with a report is in
[operations](operations.md#a-database-that-will-not-open) and
[ADR 0003](design/0003-wal-format-and-recovery.md).

## Tables

A table is a sorted run of entries, written once and never changed.

```
[data block 0][trailer] ... [data block N][trailer]
[filter block][trailer]
[index block][trailer]
[footer: 48 bytes]
```

Every block, whatever it holds, is followed by a **trailer** of 5 bytes (the `block_trailer` rows of
the [fixed layouts](#fixed-layouts)): a type byte, which is 0 for a block stored as it is (the only
value there is, and the place a compression type would go), and the masked CRC-32C of the block's
contents and that type byte. The size that a block handle gives is that of the contents, without the
trailer. A reader that finds any other type byte refuses the block.

### Data blocks and the index block

A data block holds entries in key order. Neighbouring keys in a sorted run usually begin the same
way, so an entry stores only the part of its key that differs from the key before:

```
entry = shared (varint32) ‖ non_shared (varint32) ‖ value_length (varint32) ‖ key_delta ‖ value
```

`shared` is the number of leading bytes it has in common with the previous key, and `key_delta` is the
`non_shared` bytes after them. A reader that has landed in the middle of a block cannot rebuild a key
without the keys before it, so every 16th entry (`Options::block_restart_interval`) is a **restart
point**: it shares nothing and holds its whole key. The block ends with the offsets of the restart
points and their number:

```
block = entry ‖ entry ‖ ... ‖ restart[0] ‖ ... ‖ restart[n-1] ‖ n
        (each restart a fixed32 offset from the block start; n a fixed32)
```

A seek binary-searches the restart keys, which can be read as they are, and walks forward from the
one it found. The first restart point is always at offset 0.

<!-- example:data_block -->
```text
00 05 01 61 70 70 6c 65 31              # entry 0, a restart: shared 0, non-shared 5, value length 1: "apple" "1"
05 05 01 73 61 75 63 65 32              # entry 1: shared 5 ("apple"), non-shared 5 ("sauce"), value length 1: key "applesauce", "2"
00 06 01 62 61 6e 61 6e 61 33           # entry 2, a restart: shared 0, non-shared 6: "banana" "3"
00 00 00 00 12 00 00 00                 # restart offsets: 0 and 18
02 00 00 00                             # two restart points
```

(That block was built with a restart interval of 2 and with plain keys so that it can be read. In a
table the keys are [internal keys](#internal-keys), and a data block is cut when it reaches
`Options::block_size`, 4096 bytes.)

The **index block** is a block of the same layout with one entry for each data block. Its key is the
**last internal key** of the data block (it is not shortened to a separator), and its value is the
block's handle: the offset and the size of the contents as two varint64s. The index block has a
restart point at **every** entry, so that a seek in it compares a whole key at each probe.

<!-- example:block_trailer -->
```text
00                        # type: stored as it is
fd 0a 52 dd               # masked CRC-32C of the 40 bytes of the data block above and the type byte
```

### The filter block

There is one filter for the whole table, a Bloom filter over the **user keys** (not internal keys: a
lookup asks about a user key, and the one entry has to match every version of it). A table written
with `bloom_bits_per_key = 0` has an empty filter block.

```
filter = bit array ‖ k (1 byte)
```

- The bit array has `max(n × bits_per_key, 64)` bits, rounded up to whole bytes, where `n` is the
  number of distinct user keys in the table: a key that appears in several versions is added once.
- `k`, the number of probes, is `bits_per_key × 69 / 100` rounded down (a factor of 0.69), kept between
  1 and 30.
- A key is hashed once, with the [hash below](#the-hash) and the seed `0xbc9f1d34`, to a 32-bit `h`. Its
  probes are bit `h mod B`, then `(h + delta) mod B`, and so on, where `B` is the size of the
  array in bits (its length in bytes times 8), `delta = (h >> 17) | (h << 15)` (a rotation of `h` left
  by 15) and each addition wraps at 32 bits. This is double hashing: one hash value stands in for `k`.

<!-- example:filter_block -->
```text
02 40 00 0c 80 00 d0 0f   # the 64-bit array, with the probes of "apple" and "banana" set
06                        # k: 6 probes (10 bits per key × 0.69, rounded down)
```

A reader treats a filter it cannot read, one with no bit array or a probe count above 30, as one that
says "maybe" to every key: a damaged filter costs a read of the table and never a missing key.

#### The hash

The hash of the filter must give the same value for the same bytes on every machine, for ever, since it
decides which bits of a stored filter are set. It is a 32-bit hash of `n` bytes and a seed, in 32-bit
wrapping arithmetic, with `m = 0xc6a4a793`:

```
h = seed XOR ((n × m) mod 2^32)
for each whole 4-byte word w, read little-endian, in order:
    h = h + w;   h = h × m;   h = h XOR (h >> 16)
then, if 1 to 3 bytes are left, b0 being the first of them:
    if 3 are left:      h = h + (b2 << 16)
    if 2 or 3 are left: h = h + (b1 << 8)
    h = h + b0;   h = h × m;   h = h XOR (h >> 24)
```

With no byte left over the last step does not run, and the hash of nothing is the seed. The
test recomputes it from the constants `hash_multiplier`, `hash_word_shift` and `hash_tail_shift` below, for
inputs of every length from 0 to 9 bytes, and compares it with the code.

### The footer

The footer is the one fixed-size part, at the very end, so that a reader can start from it. It names
the filter block and the index block; the index block names the data blocks. Its fields are the
`footer` rows of the [fixed layouts](#fixed-layouts).

The reader checks, in this order, because it is the order that says most: the size (`bad footer
size`), the magic number, which says whether the file is a table at all (`not an sstable (bad magic
number)`), the checksum, which says whether the footer is intact (`footer checksum mismatch`), and
last the version, which is only meaningful in a footer that is intact (`unsupported table format
version`). The last byte of every table is `0x73`, the letter *s*, the most significant byte of the
magic number.

<!-- example:footer -->
```text
00 10 00 00 00 00 00 00   # filter block offset: 4096
09 00 00 00 00 00 00 00   # filter block size: 9
0e 10 00 00 00 00 00 00   # index block offset: 4110 (4096 + 9 + the 5-byte trailer)
28 00 00 00 00 00 00 00   # index block size: 40
01 00 00 00               # format version 1
32 03 9f 92               # CRC-32C of the 36 bytes above, not masked
01 00 00 65 6c 61 68 73   # magic number 0x7368616c65000001
```

### What a reader checks

`Table::Open()` reads the footer, the index and the filter, and checks that the index is a sorted list
of keys with one handle each, every handle naming a different block in the part of the file before the
filter, in order. After that, whatever the bytes of a data block are, a read returns the entry that was
written or a `Corruption`, never a crash, a read outside the buffer or a value that was not stored. An
iterator over a table returns keys in strictly increasing order whatever the file holds, so that a
table can be fed to a table builder, which aborts on a key that is not; a block with its keys out of
order ends the iteration with `Corruption`. See [`src/io/table.h`](../src/io/table.h).

## The MANIFEST and version edits

The MANIFEST is a file in the [log format](#the-write-ahead-log): one record for each **version
edit**, replayed in order to rebuild the table set after a restart. Its **first record is a snapshot**
of the whole state, an edit that adds every live file, and every later record is the edit that one
flush or compaction made. A new MANIFEST is started every time the database opens, because the one
that was read may end in a torn record, which cannot be appended to, and so that the file does not
grow for ever.

An edit is a sequence of fields, each a tag and then the field's contents, and nothing else; an edit
with no field set is the empty string. The tag is a varint32.

| Tag | Name | Contents |
| --- | --- | --- |
| 1 | `log_number` | varint64: the number of the oldest log that still holds writes not in a table |
| 2 | `next_file_number` | varint64: the next number to give a file |
| 3 | `last_sequence` | varint64: the sequence number the database had reached when the edit was written, which counts writes that are still only in a log; recovery takes the larger of it and the last number it finds in the logs |
| 4 | `compact_pointer` | varint32 level, length-prefixed internal key: where the next compaction of the level starts |
| 5 | `deleted_file` | varint32 level, varint64 file number |
| 6 | `new_file` | varint32 level, varint64 file number, varint64 file size, length-prefixed smallest internal key, length-prefixed largest internal key |

The encoder writes the fields in tag order, and a field may repeat (a compaction deletes many files
and adds many), so one edit has one encoding. Every record that the database appends carries
`next_file_number` and `last_sequence`, taken from the counters of the database at that moment, so that the
last record is always enough to know them.

<!-- example:version_edit -->
```text
01 0c                                    # log_number = 12
02 0f                                    # next_file_number = 15
03 e8 07                                 # last_sequence = 1000
04 01 09 6d 01 09 00 00 00 00 00 00     # compact pointer: level 1, a 9-byte internal key: "m", sequence 9, a value
05 01 09                                 # deleted file: level 1, file 9
06 02 0e b8 17                           # new file: level 2, file 14, 3000 bytes
09 61 01 05 00 00 00 00 00 00           # its smallest key: "a", sequence 5, a value
09 6d 01 09 00 00 00 00 00 00           # its largest key: "m", sequence 9, a value
```

A reader reads a tag and a level as varint64 and range-checks them, so a tag of 2^32 or more is an
unknown tag and a level of 2^32 or more is a level out of range. An **unknown tag, a level of 7 or
more, or a field that ends early is `Corruption`**, with a message that starts `VersionEdit:`. There
are 7 levels, 0 to 6.

Decoding an edit is only the first check. Applying it to the table set (`ApplyEdit`, in
`src/core/version_state.cc`) refuses an edit that would break an invariant of the files: the levels
below 0 must stay sorted and disjoint, a deleted file must exist, a file number must not repeat, the
counters must not go backwards, and `log_number` must be below `next_file_number`. A MANIFEST that
replays into a state that `ApplyEdit` accepted is one the rest of the engine can trust, and one that
does not is refused when the database opens and not found later by a read.

## CURRENT

`CURRENT` is the one file that says which MANIFEST is live, and it is two lines, each ending in a
newline: the bare name of the MANIFEST, and `crc32c:` and the masked CRC-32C of the first line
**including its newline**, in eight lower-case hexadecimal digits.

<!-- text:current -->
```text
MANIFEST-000007
crc32c:76e85d4d
```

The checksum is there because `CURRENT` is one small file, read once, and one flipped bit in the
number would name another MANIFEST that is in the directory, and the database would open an older
version of itself without a word. The reader accepts that form and no other, byte for byte:

<!-- table:current_messages -->
| Message | When |
| --- | --- |
| `CURRENT file does not end with newline` | the contents are empty or do not end in a newline |
| `CURRENT file is malformed` | not a name line and then `crc32c:` and eight lower-case hex digits |
| `CURRENT checksum mismatch` | the checksum is not that of the name line |
| `CURRENT names an invalid file` | the name is not a MANIFEST name spelled as it is written |
<!-- /table:current_messages -->

They are checked in that order, so that a damaged name is reported as damage. `CURRENT` is replaced
atomically: the new contents are written to `NNNNNN.dbtmp`, synced, renamed over `CURRENT`, and the
directory is synced (`SetCurrentFile`, `src/db/filename_io.cc`). A crash leaves it naming the old
MANIFEST or the new one, whole.

## The memtable

The memtable is in memory only and is never written to a file; it is described here because its
entries are what a flush turns into table entries. An entry lives in an arena as

```
entry = varint32(internal key length) ‖ internal key ‖ varint32(value length) ‖ value
```

and a lookup key has the same layout up to the end of the internal key, which is what lets a lookup
be compared with entries directly. A flush writes each entry to a table as the internal key and the
value.

<!-- example:memtable_lookup_key -->
```text
09                        # the internal key is 9 bytes long
6b 01 07 00 00 00 00 00 00   # the internal key of the example above
```

## Compatibility

The table format version is 1, and a reader refuses a table whose footer says another, because it
cannot know what the table holds. Nothing else in the files has a version: a change to the log format,
the MANIFEST or `CURRENT` would be a change to what a database written by an earlier release means,
and there has been none. The one reserved place is the type byte of a block trailer, where a
compression type would go; adding compression would add values there and bump the table format
version, and would not change the layout above.

## Fixed layouts

The structures whose size never varies, field by field. Offsets are from the first byte of the
structure, and the fields of each follow one another with no gap. The test takes real bytes apart
with these offsets, so a field that is listed at the wrong offset reads the wrong number.

<!-- table:layouts -->
| Structure | Field | Offset | Size | Encoding |
| --- | --- | --- | --- | --- |
| `write_batch_header` | `sequence` | 0 | 8 | fixed64: the sequence number of the first operation |
| `write_batch_header` | `count` | 8 | 4 | fixed32: the number of operations |
| `wal_record_header` | `checksum` | 0 | 4 | fixed32: masked CRC-32C of the type byte and the payload |
| `wal_record_header` | `length` | 4 | 2 | fixed16: the number of payload bytes |
| `wal_record_header` | `type` | 6 | 1 | one byte: the record type |
| `block_trailer` | `type` | 0 | 1 | one byte: 0, the block is stored as it is |
| `block_trailer` | `checksum` | 1 | 4 | fixed32: masked CRC-32C of the block's contents and the type byte |
| `footer` | `filter_offset` | 0 | 8 | fixed64: where the filter block starts |
| `footer` | `filter_size` | 8 | 8 | fixed64: the length of the filter block, without its trailer |
| `footer` | `index_offset` | 16 | 8 | fixed64: where the index block starts |
| `footer` | `index_size` | 24 | 8 | fixed64: the length of the index block, without its trailer |
| `footer` | `format_version` | 32 | 4 | fixed32: 1 |
| `footer` | `checksum` | 36 | 4 | fixed32: CRC-32C of bytes 0 to 35, not masked |
| `footer` | `magic` | 40 | 8 | fixed64: 0x7368616c65000001 |
<!-- /table:layouts -->

## Constants

Every number that the layouts above depend on. Each is compared with the code. The names are the
code's own where the code has one (`log::kBlockSize` is `kBlockSize` in namespace `shale::log`); a
name in lower case is a number the code does not name, and the test finds it by looking at what the
code does, for example the first byte of an encoded edit.

<!-- table:constants -->
| Constant | Value | Meaning |
| --- | --- | --- |
| `log::kBlockSize` | 32768 | Size of a log block. |
| `log::kHeaderSize` | 7 | Size of a log record's header. |
| `log::kZeroType` | 0 | Record type that is never written. |
| `log::kFullType` | 1 | A whole record in one fragment. |
| `log::kFirstType` | 2 | First fragment of a record. |
| `log::kMiddleType` | 3 | A fragment in the middle. |
| `log::kLastType` | 4 | Last fragment of a record. |
| `kBlockTrailerSize` | 5 | Size of the trailer after every table block. |
| `block_type_uncompressed` | 0 | The one value of a trailer's type byte. |
| `kFooterSize` | 48 | Size of a table's footer. |
| `kTableFormatVersion` | 1 | Version in the footer. |
| `kTableMagicNumber` | 0x7368616c65000001 | Last 8 bytes of every table. |
| `index_block_restart_interval` | 1 | Entries between restart points in an index block. |
| `Options::block_size` | 4096 | Default size at which a data block is cut. |
| `Options::block_restart_interval` | 16 | Default entries between restart points in a data block. |
| `Options::bloom_bits_per_key` | 10 | Default bits of filter for each key. |
| `kInternalKeyTagSize` | 8 | Size of the tag that ends an internal key. |
| `kMaxSequenceNumber` | 0xffffffffffffff | Largest sequence number, 2^56 - 1. |
| `ValueType::kDeletion` | 0 | Type of a tombstone. |
| `ValueType::kValue` | 1 | Type of a value. |
| `kNumLevels` | 7 | Levels 0 to 6. |
| `kMaxFileNumber` | 0x100000000000000 | A file number is below 2^56. |
| `kMaxKeySize` | 65536 | Longest key. |
| `kMaxValueSize` | 67108864 | Longest value. |
| `write_batch_header_size` | 12 | Size of a batch's header. |
| `write_batch_tag_deletion` | 0 | Tag of a deletion in a batch. |
| `write_batch_tag_put` | 1 | Tag of a put in a batch. |
| `tag_log_number` | 1 | Version edit tag. |
| `tag_next_file_number` | 2 | Version edit tag. |
| `tag_last_sequence` | 3 | Version edit tag. |
| `tag_compact_pointer` | 4 | Version edit tag. |
| `tag_deleted_file` | 5 | Version edit tag. |
| `tag_new_file` | 6 | Version edit tag. |
| `crc32c_mask_rotate` | 15 | The CRC is rotated right by this many bits when masked. |
| `crc32c_mask_delta` | 0xa282ead8 | What is added after the rotation. |
| `crc32c_check_value` | 0xe3069283 | CRC-32C of the nine ASCII digits `123456789`, which fixes the polynomial. |
| `bloom_hash_seed` | 0xbc9f1d34 | Seed of the filter's hash. |
| `hash_multiplier` | 0xc6a4a793 | The multiplier `m` of the hash. |
| `hash_word_shift` | 16 | `h` is XORed with itself shifted right by this much after each whole word. |
| `hash_tail_shift` | 24 | The same, after the last 1 to 3 bytes. |
| `bloom_probe_delta_rotate` | 15 | The probe step is the hash rotated left by this many bits. |
| `bloom_probe_percent` | 69 | `k` is `bits_per_key` times this, over 100, rounded down. |
| `bloom_min_bits` | 64 | The smallest bit array of a filter. |
| `bloom_max_probes` | 30 | The most probes; a larger count in a file is a filter the reader does not know. |
<!-- /table:constants -->
