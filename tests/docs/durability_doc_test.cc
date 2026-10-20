// Keeps the statement of the durability contract the same in every place that makes it, and the
// tests that the documents name the tests that exist.
//
// The contract is written twice, in the README ("What it promises") and in ADR 0004, and the order
// of syncs and the checks that back it are described in four more documents. A sentence that one of
// them changed and the others did not is a reader told two things, and a test name that was changed
// in the code and not in the document is a claim that nobody can check. So:
//
//   - both tables of the contract have the rows D1 to D7, in that order;
//   - the row D6, which is the one that has been restated, names in both places the three traces of
//     a crash that an open tolerates whatever `paranoid_checks` says (an end cut off, zeros from a
//     record boundary to the end of the file, in the MANIFEST too, and a gap in the sequence
//     numbers), that what is set aside is a `.dropped` file that the engine does not delete, that
//     no synced write is lost, and that garbage, zeros followed by another byte and zeros inside a
//     record are still defects;
//   - every test that the table of checks in ADR 0004 names is a test of the source tree, and every
//     file it names is a file;
//   - every long identifier that a document puts between backticks is one that the source tree has,
//     and a name of the form `Suite.Name` is a test of that suite.
//
// The tables are found by the HTML comments around them; see docs/doc_tables.h.

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "docs/doc_tables.h"

namespace shale {
namespace {

namespace fs = std::filesystem;

using doc_tables::LoadLines;
using doc_tables::TableRows;

fs::path Root() { return fs::path(SHALE_SOURCE_DIR); }

std::string ReadFile(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  EXPECT_TRUE(in.is_open()) << "cannot open " << path;
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// The files under `dir` (relative to the root of the source tree) whose names end in one of
// `extensions`.
std::vector<fs::path> FilesUnder(const std::string& dir, const std::set<std::string>& extensions) {
  std::vector<fs::path> files;
  const fs::path base = Root() / dir;
  if (!fs::is_directory(base)) {
    ADD_FAILURE() << base << " is not a directory";
    return files;
  }
  for (const auto& entry : fs::recursive_directory_iterator(base)) {
    if (entry.is_regular_file() && extensions.count(entry.path().extension().string()) != 0) {
      files.push_back(entry.path());
    }
  }
  return files;
}

// What the contract tables hold: the text of each row, by its label ("D1" to "D7").
using Contract = std::vector<std::pair<std::string, std::string>>;

std::string Plain(const std::string& cell) {
  std::string out;
  for (const char c : cell) {
    if (c != '*') {
      out += c;
    }
  }
  return out;
}

Contract ReadContract(const fs::path& document, const std::string& first_column) {
  Contract contract;
  for (const auto& row :
       TableRows(LoadLines(document.string()), "contract", {first_column, "The promise"})) {
    contract.emplace_back(Plain(row[0]), row[1]);
  }
  return contract;
}

// The two places that state the contract, as the path of the document and the header of the first
// column of its table.
std::vector<std::pair<std::string, std::string>> Statements() {
  return {{"README.md", ""}, {"docs/design/0004-durability.md", "#"}};
}

TEST(DurabilityDocTest, BothStatementsOfTheContractHaveD1ToD7InOrder) {
  const std::vector<std::string> expected = {"D1", "D2", "D3", "D4", "D5", "D6", "D7"};
  for (const auto& [document, column] : Statements()) {
    const Contract contract = ReadContract(Root() / document, column);
    std::vector<std::string> labels;
    for (const auto& row : contract) {
      labels.push_back(row.first);
    }
    EXPECT_EQ(labels, expected) << "the contract in " << document;
  }
}

// The row D6 is held to the words that carry its meaning, in both places. Where the words are is
// not held: a document may phrase it as it likes, as long as it says each of these things.
TEST(DurabilityDocTest, D6NamesTheTolerableTracesAndWhatBecomesOfTheLogsSetAside) {
  const std::vector<std::string> needed = {
      "paranoid_checks",
      "cut off by the end of a file",
      "gap in the sequence numbers at the start of a log",
      "zeros from a record boundary to the end of the file",
      "MANIFEST",
      ".dropped",
      "deleted",
      "acknowledged as synced",
      "garbage",
      "zeros followed by",
      "inside a record",
  };
  for (const auto& [document, column] : Statements()) {
    const Contract contract = ReadContract(Root() / document, column);
    std::string d6;
    for (const auto& row : contract) {
      if (row.first == "D6") {
        d6 = row.second;
      }
    }
    ASSERT_FALSE(d6.empty()) << document << " has no row D6";
    for (const std::string& phrase : needed) {
      EXPECT_NE(d6.find(phrase), std::string::npos)
          << "the row D6 of " << document << " does not say '" << phrase << "'";
    }
  }
}

// The names of the tests of the source tree: "Suite.Name" for each TEST, TEST_F or TEST_P.
std::set<std::string> TestNames() {
  static const std::regex declaration(R"(\bTEST(?:_F|_P)?\s*\(\s*(\w+)\s*,\s*(\w+)\s*\))");
  std::set<std::string> names;
  for (const fs::path& file : FilesUnder("tests", {".cc"})) {
    const std::string text = ReadFile(file);
    for (std::sregex_iterator it(text.begin(), text.end(), declaration), end; it != end; ++it) {
      names.insert((*it)[1].str() + "." + (*it)[2].str());
    }
  }
  return names;
}

// The identifiers that the source tree uses anywhere, which is where a document's name for a
// function or a type has to be.
std::set<std::string> Identifiers() {
  static const std::regex word(R"([A-Za-z_][A-Za-z0-9_]*)");
  std::set<std::string> names;
  for (const char* dir : {"include", "src", "tests", "tools", "bench", "fuzz", "examples"}) {
    for (const fs::path& file : FilesUnder(dir, {".cc", ".h"})) {
      const std::string text = ReadFile(file);
      for (std::sregex_iterator it(text.begin(), text.end(), word), end; it != end; ++it) {
        names.insert(it->str());
      }
    }
  }
  return names;
}

// The things between backticks in `text`.
std::vector<std::string> Quoted(const std::string& text) {
  static const std::regex quoted(R"(`([^`\n]+)`)");
  std::vector<std::string> out;
  for (std::sregex_iterator it(text.begin(), text.end(), quoted), end; it != end; ++it) {
    out.push_back((*it)[1].str());
  }
  return out;
}

// Whether `token` is a name in the style of the tests and the functions, CamelCase from one end to
// the other, optionally preceded by the suite as `Suite.`.
bool IsCamelName(const std::string& token) {
  static const std::regex camel(R"((?:[A-Z][A-Za-z0-9]*\.)?[A-Z][A-Za-z0-9]*[a-z][A-Za-z0-9]*)");
  return std::regex_match(token, camel);
}

TEST(DurabilityDocTest, TheChecksTableNamesTestsAndFilesThatExist) {
  const fs::path adr = Root() / "docs/design/0004-durability.md";
  const auto rows = TableRows(LoadLines(adr.string()), "checks", {"", "By"});
  ASSERT_EQ(rows.size(), size_t{5}) << "D1 to D3 together, then D4, D5, D6 and D7";

  const std::set<std::string> tests = TestNames();
  ASSERT_FALSE(tests.empty());
  std::set<std::string> bare;
  for (const std::string& name : tests) {
    bare.insert(name.substr(name.find('.') + 1));
  }
  size_t named = 0;
  size_t files = 0;
  for (const auto& row : rows) {
    for (const std::string& token : Quoted(row[1])) {
      if (token.rfind("tests/", 0) == 0) {
        files++;
        EXPECT_TRUE(fs::is_regular_file(Root() / token))
            << "the checks table of ADR 0004 names the file " << token << ", which is not there";
      } else if (IsCamelName(token) && token.size() >= 12) {
        named++;
        EXPECT_TRUE(token.find('.') != std::string::npos ? tests.count(token) != 0
                                                         : bare.count(token) != 0)
            << "the checks table of ADR 0004 names the test " << token
            << ", which is not a test of the source tree";
      }
    }
  }
  // The row D6 names the two tests of the gap and those of the zeros, the MANIFEST from CI among
  // them; if they stop being named here, so does the claim.
  EXPECT_GE(named, size_t{8});
  EXPECT_GE(files, size_t{9});
  for (const char* test : {"ALaterLogIsSetAsideWhenAnEarlierLogLostItsEnd",
                           "AFirstLogThatLostEverythingIsCaughtByTheManifestsSequenceNumber",
                           "AZeroFilledTailOfALogIsDroppedByAParanoidOpen",
                           "AZeroFilledTailOfTheManifestIsDroppedByAParanoidOpen",
                           "ZerosFollowedByAnyOtherByteAreStillADefect",
                           "TheManifestThatAKillOnMacOs14LeftEndingInZerosRecovers"}) {
    EXPECT_NE(rows[3][1].find(test), std::string::npos) << "the row D6 does not name " << test;
  }
}

TEST(DurabilityDocTest, EveryLongNameInTheDocumentsIsAnIdentifierOfTheSource) {
  const std::set<std::string> tests = TestNames();
  const std::set<std::string> identifiers = Identifiers();
  ASSERT_FALSE(identifiers.empty());

  std::vector<fs::path> documents = FilesUnder("docs", {".md"});
  documents.push_back(Root() / "README.md");
  size_t checked = 0;
  for (const fs::path& document : documents) {
    for (const std::string& token : Quoted(ReadFile(document))) {
      if (!IsCamelName(token) || token.size() < 25) {
        continue;
      }
      checked++;
      const size_t dot = token.find('.');
      const std::string name = dot == std::string::npos ? token : token.substr(dot + 1);
      EXPECT_TRUE(identifiers.count(name) != 0)
          << document.filename() << " names " << token << ", which the source does not have";
      if (dot != std::string::npos) {
        EXPECT_TRUE(tests.count(token) != 0)
            << document.filename() << " names the test " << token << ", which is not a test";
      }
    }
  }
  EXPECT_GE(checked, size_t{20});
}

}  // namespace
}  // namespace shale
