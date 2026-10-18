#include <iostream>
#include <memory>
#include <string>

#include "shale/db.h"

int main() {
  shale::Options options;
  options.create_if_missing = true;
  std::unique_ptr<shale::DB> db;
  shale::Status s = shale::DB::Open(options, "/tmp/shale-quickstart", &db);
  std::string value;
  if (s.ok()) s = db->Put(shale::WriteOptions(), "greeting", "hello");
  if (s.ok()) s = db->Get(shale::ReadOptions(), "greeting", &value);
  std::cout << (s.ok() ? value : s.ToString()) << '\n';
  return s.ok() ? 0 : 1;
}
