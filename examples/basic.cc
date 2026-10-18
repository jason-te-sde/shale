// A first program against an installed shale: it opens a database, writes three keys in one batch,
// reads one, walks all of them in order, and reads a key as of a snapshot taken before it was
// overwritten.
//
// Usage: basic <database directory>
//
// The directory is created if it is not there. Running the program again on the same directory
// prints the same thing, since the batch sets the keys it reads back to the same values.

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include "shale/db.h"

namespace {

// Every call of shale returns a Status. An example that stops at the first error shows the calls
// and not what a program does about the errors.
void Must(const shale::Status& status, const std::string& what) {
  if (!status.ok()) {
    std::cerr << "basic: " << what << ": " << status.ToString() << '\n';
    std::exit(1);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: basic <database directory>\n";
    return 2;
  }

  shale::Options options;
  options.create_if_missing = true;
  std::unique_ptr<shale::DB> db;
  Must(shale::DB::Open(options, argv[1], &db), "open");

  // The three puts are one write: after a crash, all of them are there or none is.
  shale::WriteBatch batch;
  batch.Put("a", "1");
  batch.Put("b", "2");
  batch.Put("c", "3");
  Must(db->Write(shale::WriteOptions(), &batch), "write the batch");

  std::string value;
  Must(db->Get(shale::ReadOptions(), "b", &value), "get b");
  std::cout << "get b: " << value << '\n';

  // An iterator is in key order. It is destroyed before the database, as the database requires.
  std::cout << "scan:\n";
  {
    std::unique_ptr<shale::Iterator> it = db->NewIterator(shale::ReadOptions());
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
      std::cout << "  " << it->key() << " = " << it->value() << '\n';
    }
    // The loop also ends when a table could not be read, which status() says.
    Must(it->status(), "scan");
  }

  // A snapshot keeps seeing what the database held when it was taken, whatever is written later.
  const shale::Snapshot* snapshot = db->GetSnapshot();
  Must(db->Put(shale::WriteOptions(), "a", "10"), "overwrite a");

  shale::ReadOptions at_snapshot;
  at_snapshot.snapshot = snapshot;
  Must(db->Get(at_snapshot, "a", &value), "get a at the snapshot");
  std::cout << "get a at the snapshot: " << value << '\n';
  Must(db->Get(shale::ReadOptions(), "a", &value), "get a");
  std::cout << "get a now: " << value << '\n';

  // A snapshot goes back to the database that made it, before the database is closed.
  db->ReleaseSnapshot(snapshot);
  return 0;
}
