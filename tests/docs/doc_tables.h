#ifndef SHALE_TESTS_DOCS_DOC_TABLES_H_
#define SHALE_TESTS_DOCS_DOC_TABLES_H_

// Reading the tables and the examples that a document carries for a test to check.
//
// A document that states numbers in prose drifts from the code. The documents that are held to the
// code (docs/format.md and docs/operations.md) put those numbers where a test can read them: a
// Markdown table between two HTML comments, `<!-- table:NAME -->` and `<!-- /table:NAME -->`, which
// a reader of the rendered page does not see, and fenced blocks that follow a `<!-- KIND:NAME -->`
// line. This header reads them. What each number is compared with is the test's business.
//
// Every function reports a problem with the document as a test failure, through ADD_FAILURE, and
// returns what it could read, so that a document that has lost its table fails the test and is not
// skipped by it.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace shale::doc_tables {

inline std::string Trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r')) {
    begin++;
  }
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
    end--;
  }
  return std::string(text.substr(begin, end - begin));
}

// A cell written as `name` is the name.
inline std::string Unquote(const std::string& cell) {
  if (cell.size() >= 2 && cell.front() == '`' && cell.back() == '`') {
    return cell.substr(1, cell.size() - 2);
  }
  return cell;
}

inline bool StartsWith(const std::string& text, std::string_view prefix) {
  return text.compare(0, prefix.size(), prefix) == 0;
}

// The cells of a table row, "| a | b |" being {"a", "b"}.
inline std::vector<std::string> SplitCells(const std::string& line) {
  std::vector<std::string> cells;
  size_t bar = line.find('|');
  while (bar != std::string::npos) {
    const size_t next = line.find('|', bar + 1);
    if (next == std::string::npos) {
      break;
    }
    cells.push_back(Unquote(Trim(std::string_view(line).substr(bar + 1, next - bar - 1))));
    bar = next;
  }
  return cells;
}

// A number as the document writes it: decimal, or hexadecimal with a 0x. Anything else, a trailing
// word included, is a mistake in the document and fails the test.
inline bool ParseNumber(const std::string& text, uint64_t* value) {
  if (text.empty()) {
    return false;
  }
  const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  const size_t first = hex ? 2 : 0;
  uint64_t result = 0;
  for (size_t i = first; i < text.size(); i++) {
    const char c = text[i];
    uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<uint64_t>(c - '0');
    } else if (hex && c >= 'a' && c <= 'f') {
      digit = static_cast<uint64_t>(c - 'a' + 10);
    } else if (hex && c >= 'A' && c <= 'F') {
      digit = static_cast<uint64_t>(c - 'A' + 10);
    } else {
      return false;
    }
    const uint64_t base = hex ? 16 : 10;
    if (result > (UINT64_MAX - digit) / base) {
      return false;
    }
    result = result * base + digit;
  }
  *value = result;
  return true;
}

inline std::vector<std::string> LoadLines(const std::string& path) {
  std::vector<std::string> lines;
  std::ifstream in(path);
  if (!in.is_open()) {
    ADD_FAILURE() << "cannot open " << path;
    return lines;
  }
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

// The rows of the table between `<!-- table:NAME -->` and `<!-- /table:NAME -->`, without its
// header and separator. The header must be the one given, so that a column that is added or
// reordered in the document cannot be read as another.
inline std::vector<std::vector<std::string>> TableRows(const std::vector<std::string>& lines,
                                                       const std::string& name,
                                                       const std::vector<std::string>& header) {
  std::vector<std::vector<std::string>> rows;
  const std::string open = "<!-- table:" + name + " -->";
  const std::string close = "<!-- /table:" + name + " -->";
  size_t i = 0;
  while (i < lines.size() && Trim(lines[i]) != open) {
    i++;
  }
  if (i == lines.size()) {
    ADD_FAILURE() << "the document has no '" << open << "' marker";
    return rows;
  }
  i++;
  std::vector<std::vector<std::string>> table;
  for (; i < lines.size() && Trim(lines[i]) != close; i++) {
    if (StartsWith(Trim(lines[i]), "|")) {
      table.push_back(SplitCells(Trim(lines[i])));
    }
  }
  if (i == lines.size()) {
    ADD_FAILURE() << "the document has no '" << close << "' marker";
    return rows;
  }
  if (table.size() < 3) {
    ADD_FAILURE() << "the table '" << name << "' has no rows";
    return rows;
  }
  EXPECT_EQ(table[0], header) << "the header of the table '" << name << "'";
  for (size_t r = 2; r < table.size(); r++) {
    EXPECT_EQ(table[r].size(), header.size()) << "a row of the table '" << name << "'";
    if (table[r].size() == header.size()) {
      rows.push_back(table[r]);
    }
  }
  return rows;
}

// The fenced blocks that follow `<!-- KIND:NAME -->` lines, by name: the lines of each block, which
// start on the line after the opening fence and stop before the closing one.
inline std::map<std::string, std::vector<std::string>> FencedBlocks(
    const std::vector<std::string>& lines, const std::string& kind) {
  std::map<std::string, std::vector<std::string>> blocks;
  const std::string prefix = "<!-- " + kind + ":";
  for (size_t i = 0; i < lines.size(); i++) {
    const std::string line = Trim(lines[i]);
    if (!StartsWith(line, prefix) || line.size() < prefix.size() + 4 ||
        line.compare(line.size() - 4, 4, " -->") != 0) {
      continue;
    }
    const std::string name = line.substr(prefix.size(), line.size() - prefix.size() - 4);
    if (i + 1 >= lines.size() || !StartsWith(Trim(lines[i + 1]), "```")) {
      ADD_FAILURE() << "'" << line << "' is not followed by a fenced block";
      continue;
    }
    std::vector<std::string> body;
    size_t j = i + 2;
    for (; j < lines.size() && !StartsWith(Trim(lines[j]), "```"); j++) {
      body.push_back(lines[j]);
    }
    if (j == lines.size()) {
      ADD_FAILURE() << "the fenced block of '" << line << "' is not closed";
      continue;
    }
    if (!blocks.emplace(name, std::move(body)).second) {
      ADD_FAILURE() << "the " << kind << " '" << name << "' appears twice";
    }
  }
  return blocks;
}

// The bytes of a hex dump: whitespace-separated pairs of hex digits, and anything after a '#' on a
// line is a note for the reader.
inline std::string HexBytes(const std::string& name, const std::vector<std::string>& body) {
  std::string bytes;
  for (const std::string& line : body) {
    const std::string code = line.substr(0, line.find('#'));
    size_t pos = 0;
    while (pos < code.size()) {
      while (pos < code.size() && (code[pos] == ' ' || code[pos] == '\t' || code[pos] == '\r')) {
        pos++;
      }
      if (pos == code.size()) {
        break;
      }
      size_t end = pos;
      while (end < code.size() && code[end] != ' ' && code[end] != '\t' && code[end] != '\r') {
        end++;
      }
      uint64_t value = 0;
      const std::string token = code.substr(pos, end - pos);
      if (token.size() != 2 || !ParseNumber("0x" + token, &value)) {
        ADD_FAILURE() << "the example '" << name << "' has '" << token << "', which is not a byte";
        return "";
      }
      bytes.push_back(static_cast<char>(value));
      pos = end;
    }
  }
  return bytes;
}

inline std::string Hex(std::string_view bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  for (const char c : bytes) {
    const auto b = static_cast<uint8_t>(c);
    if (!text.empty()) {
      text.push_back(' ');
    }
    text.push_back(kDigits[b >> 4]);
    text.push_back(kDigits[b & 15]);
  }
  return text;
}

}  // namespace shale::doc_tables

#endif  // SHALE_TESTS_DOCS_DOC_TABLES_H_
