// Keeps the code in the README the code that is built.
//
// The README shows a program of fifteen lines and the three lines of CMake that a project needs to
// use it. A snippet in a README that has been copied by hand is a snippet that stops compiling the
// day the API changes, and nobody notices until a reader pastes it. So the program is a file,
// examples/quickstart.cc, that CI builds against an installed copy of the library and runs, and the
// README's copy of it must be that file, byte for byte; and the CMake lines must be lines of
// examples/CMakeLists.txt, which is built in the same job.
//
// The blocks are found by the HTML comments before them (`<!-- example:quickstart -->` and
// `<!-- example:quickstart_cmake -->`); see docs/doc_tables.h.

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "docs/doc_tables.h"

namespace shale {
namespace {

using doc_tables::FencedBlocks;
using doc_tables::LoadLines;
using doc_tables::Trim;

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open()) << "cannot open " << path;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

TEST(ReadmeTest, TheProgramInTheReadmeIsTheExampleThatIsBuilt) {
  const std::vector<std::string> lines = LoadLines(SHALE_README_PATH);
  const auto blocks = FencedBlocks(lines, "example");
  const auto it = blocks.find("quickstart");
  ASSERT_TRUE(it != blocks.end()) << "README.md has no '<!-- example:quickstart -->' block";
  std::string shown;
  for (const std::string& line : it->second) {
    shown += line + "\n";
  }
  const std::string built = ReadFile(std::string(SHALE_EXAMPLES_DIR) + "/quickstart.cc");
  EXPECT_FALSE(built.empty());
  EXPECT_EQ(shown, built) << "the README's program is not examples/quickstart.cc";

  // Fifteen lines of code, as the README says.
  size_t code_lines = 0;
  for (const std::string& line : it->second) {
    if (!Trim(line).empty()) {
      code_lines++;
    }
  }
  EXPECT_EQ(code_lines, size_t{15});
}

TEST(ReadmeTest, TheCMakeLinesInTheReadmeAreLinesOfTheExample) {
  const std::vector<std::string> readme = LoadLines(SHALE_README_PATH);
  const auto blocks = FencedBlocks(readme, "example");
  const auto it = blocks.find("quickstart_cmake");
  ASSERT_TRUE(it != blocks.end()) << "README.md has no '<!-- example:quickstart_cmake -->' block";

  std::set<std::string> built;
  for (const std::string& line : LoadLines(std::string(SHALE_EXAMPLES_DIR) + "/CMakeLists.txt")) {
    built.insert(Trim(line));
  }
  size_t shown = 0;
  for (const std::string& line : it->second) {
    const std::string text = Trim(line);
    if (text.empty()) {
      continue;
    }
    shown++;
    EXPECT_TRUE(built.count(text) != 0)
        << "'" << text << "' is not a line of examples/CMakeLists.txt";
  }
  EXPECT_EQ(shown, size_t{3});
}

}  // namespace
}  // namespace shale
