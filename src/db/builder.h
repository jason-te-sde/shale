#ifndef SHALE_DB_BUILDER_H_
#define SHALE_DB_BUILDER_H_

// Turning a sorted run of entries into a table file: what a flush does with a memtable and a
// compaction with its merged input.

#include <string>

#include "core/version_edit.h"
#include "db/table_cache.h"
#include "io/table_builder.h"
#include "shale/env.h"
#include "shale/iterator.h"
#include "shale/status.h"

namespace shale {

/// Writes the entries of `iter`, from its first to its last, to table file `meta->number` in
/// `dbname`, and fills in `meta`.
///
/// The file is finished, synced and closed, and then opened through `table_cache` to see that it is
/// a table that can be read, and only then is the directory synced, so that when this returns OK
/// the file is whole, durable and named, and an edit that adds it to the MANIFEST can be made
/// durable without a crash being able to leave the MANIFEST naming a file that is not there.
///
/// On success `meta->file_size`, `smallest` and `largest` describe the file: its size, and its
/// first and last internal keys. `meta->number` is the caller's and is not changed. `iter` is
/// positioned by this call and its keys must be internal keys in strictly increasing order, as a
/// memtable's and a merging iterator's are; the TableBuilder aborts on one that is not.
///
/// An iterator with no entries creates no file: the call returns OK and leaves `meta->file_size`
/// at 0, which the caller reads as "nothing was written". An iterator that fails, even after it
/// returned entries, makes the call fail with its error, since a table of the entries it did return
/// would silently lose the rest.
///
/// Whenever the call returns an error, or builds nothing, `meta->file_size` is 0 and its keys are
/// empty, and the partial file, if any was created, has been removed and dropped from
/// `table_cache`.
/// @param dbname the database directory.
/// @param env the Env to write through.
/// @param options the shape of the table.
/// @param table_cache the cache the verifying open goes through, and so where the table is left
///   open for the reads that follow.
/// @param iter the entries to write.
/// @param meta receives the description; `meta->number` names the file.
Status BuildTable(const std::string& dbname, Env* env, const TableBuilderOptions& options,
                  TableCache* table_cache, Iterator* iter, FileMetaData* meta);

}  // namespace shale

#endif  // SHALE_DB_BUILDER_H_
