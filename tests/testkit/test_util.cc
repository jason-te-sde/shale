#include "testkit/test_util.h"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "util/check.h"
#include "util/random.h"

namespace shale {

std::string RandomString(Random* rnd, size_t len) {
  std::string result(len, '\0');
  for (char& c : result) {
    c = static_cast<char>(' ' + rnd->Uniform(95));
  }
  return result;
}

std::string RandomBinaryString(Random* rnd, size_t len) {
  std::string result(len, '\0');
  for (char& c : result) {
    c = static_cast<char>(rnd->Uniform(256));
  }
  return result;
}

std::string NewTempDir(const std::string& prefix) {
  std::string pattern = testing::TempDir();
  if (pattern.empty() || pattern.back() != '/') {
    pattern.push_back('/');
  }
  pattern += prefix;
  pattern += "XXXXXX";
  // mkdtemp rewrites the template in place, so it needs a writable, NUL-terminated buffer.
  std::vector<char> buffer(pattern.begin(), pattern.end());
  buffer.push_back('\0');
  const char* created = mkdtemp(buffer.data());
  SHALE_CHECK(created != nullptr, "mkdtemp failed for " + pattern + ": " + std::strerror(errno));
  return created;
}

}  // namespace shale
