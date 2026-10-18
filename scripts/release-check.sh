#!/usr/bin/env bash
# scripts/release-check.sh [--root DIR] [--link-base URL] <command> [argument]
#
# What the release workflow (.github/workflows/release.yml) asks of the source tree, in a script, so
# that a maintainer can ask it the same things before a tag is pushed.
#
#   version         prints the version that CMakeLists.txt gives the project.
#   check TAG       exits 0 when TAG is "v" and that version, and otherwise exits 1 after printing
#                   the tag and the version, so that a mismatch can be read off the log. The tag is
#                   compared as it is: "0.1.0", "V0.1.0", "v0.1" and "v0.1.0-rc1" are all refused.
#   notes VERSION   prints the section of CHANGELOG.md headed "## [VERSION]", which is what the
#                   release page says. VERSION has no leading "v". Exits 1 when the changelog has no
#                   such section, has two, or has an empty one: a release that would say nothing about
#                   itself is refused.
#
# --root DIR is the tree to read (default: the repository this script is in). --link-base URL makes
# notes rewrite every relative link to URL followed by the path: a release page is not a file of the
# repository, so a link such as docs/format.md is a dead link there. Links that have a scheme,
# links that are only a #fragment, and links inside a fenced block are left as they are. An image is a
# link like the others, so a relative image becomes the address of its page in the repository and not
# of the picture: a release that shows one needs an absolute URL for it.
#
# The version is read from the project() call, and from nowhere else:
#   - the command name is matched in any case, and commands that only end in "project" are not it;
#   - VERSION is a keyword of that call, so the VERSION of cmake_minimum_required(), a set(VERSION)
#     and the word inside a quoted DESCRIPTION are not it;
#   - comments, and # and parentheses inside quoted strings, are understood; bracket arguments and
#     bracket comments are not, which is why a second project() call, whether commented out in a
#     bracket or not, is an error and not a guess;
#   - the version must be a literal of one to four numbers, quoted or not. A variable would need
#     CMake to evaluate it, and a script that guessed would tag the wrong release.
#
# A notes section starts at its heading and ends at the next "## " heading, or at the first link
# definition ("[name]: url" at the start of a line), which is where Keep a Changelog puts the compare
# links, so that the last section does not take them with it. Blank lines at both ends are dropped.
# A section that uses reference-style links would lose their definitions: write its links inline.
# A heading or a definition inside a fenced block is text.
#
# Exit status: 0 the answer is on stdout, 1 the release is refused (a tag that is not the version, a
# changelog without its section), 2 the question could not be asked (a bad invocation, a file that
# cannot be read, a version that is not a literal). A check that could not look must not say "ok".
set -uo pipefail
export LC_ALL=C

usage() {
  if [ "$#" -gt 0 ]; then
    echo "release-check: $1" >&2
  fi
  cat >&2 <<'EOF'
usage: release-check.sh [--root DIR] [--link-base URL] version
       release-check.sh [--root DIR] [--link-base URL] check TAG
       release-check.sh [--root DIR] [--link-base URL] notes VERSION
EOF
  exit 2
}

root=""
link_base=""
while [ "$#" -gt 0 ]; do
  case "$1" in
    --root)
      [ "$#" -ge 2 ] || usage "--root needs a directory"
      root=$2
      shift 2
      ;;
    --link-base)
      [ "$#" -ge 2 ] || usage "--link-base needs a URL"
      link_base=$2
      shift 2
      ;;
    -*) usage "no such option: $1" ;;
    *) break ;;
  esac
done
[ "$#" -ge 1 ] || usage
command_name=$1
shift

if [ -z "$root" ]; then
  root="$(cd "$(dirname "$0")/.." && pwd)"
fi

version_pattern='^[0-9]+(\.[0-9]+){0,3}$'

# read_file <path>: the file must exist and be readable, or nothing can be checked.
read_file() {
  if [ ! -f "$1" ] || [ ! -r "$1" ]; then
    echo "release-check: cannot read $1" >&2
    exit 2
  fi
}

# project_version: prints the version of the project() call of CMakeLists.txt. The file is cut into
# tokens first (words, quoted strings and parentheses, with comments dropped), and the call is read
# from the tokens, so that its layout does not matter.
project_version() {
  local file="$root/CMakeLists.txt"
  read_file "$file"
  awk '
    function fail(message) {
      print "release-check: " message > "/dev/stderr"
      exit 2
    }
    function push(text, is_quoted) {
      ntok++
      tok[ntok] = text
      quoted[ntok] = is_quoted
    }
    function flush_word() {
      if (word != "") {
        push(word, 0)
        word = ""
      }
    }
    {
      line = $0
      sub(/\r$/, "", line)
      n = length(line)
      for (i = 1; i <= n; i++) {
        c = substr(line, i, 1)
        if (c == "\\") {
          i++
          word = word substr(line, i, 1)
        } else if (inq) {
          if (c == "\"") {
            push(word, 1)
            word = ""
            inq = 0
          } else {
            word = word c
          }
        } else if (c == "#") {
          break
        } else if (c == "\"") {
          flush_word()
          inq = 1
        } else if (c == "(" || c == ")") {
          flush_word()
          push(c, 0)
        } else if (c == " " || c == "\t") {
          flush_word()
        } else {
          word = word c
        }
      }
      if (inq) {
        word = word "\n"
      } else {
        flush_word()
      }
    }
    END {
      if (inq) {
        fail("CMakeLists.txt ends inside a quoted argument")
      }
      calls = 0
      for (t = 1; t < ntok; t++) {
        if (!quoted[t] && tolower(tok[t]) == "project" && !quoted[t + 1] && tok[t + 1] == "(") {
          calls++
          first = t + 2
        }
      }
      if (calls == 0) {
        fail("CMakeLists.txt has no project() call")
      }
      if (calls > 1) {
        fail("CMakeLists.txt has more than one project() call, so which is the project is a guess")
      }
      closed = 0
      keywords = 0
      for (t = first; t <= ntok; t++) {
        if (!quoted[t] && tok[t] == ")") {
          closed = 1
          break
        }
        if (!quoted[t] && tok[t] == "VERSION") {
          keywords++
          if (keywords == 1) {
            value_at = t + 1
          }
        }
      }
      if (!closed) {
        fail("the project() call is never closed")
      }
      if (keywords == 0) {
        fail("the project() call has no VERSION")
      }
      if (keywords > 1) {
        fail("the project() call names VERSION more than once")
      }
      value = ""
      if (value_at <= ntok && (quoted[value_at] || tok[value_at] != ")")) {
        value = tok[value_at]
      }
      if (value !~ /^[0-9]+(\.[0-9]+(\.[0-9]+(\.[0-9]+)?)?)?$/) {
        fail("the VERSION of project() is not a literal version of one to four numbers: [" value "]")
      }
      print value
    }' "$file"
}

# check_tag <tag>: the tag must be exactly "v" and the version.
check_tag() {
  local tag=$1 version expected named
  [ -n "$tag" ] || usage "check needs a tag"
  version="$(project_version)" || exit "$?"
  expected="v$version"
  if [ "$tag" = "$expected" ]; then
    echo "release-check: the tag $tag is the project version"
    return 0
  fi
  case "$tag" in
    v*) named="names version ${tag#v}" ;;
    *) named="does not start with v" ;;
  esac
  {
    echo "release-check: refusing the tag $tag: it is not the version of the project"
    echo "release-check:   tag:             $tag ($named)"
    echo "release-check:   CMakeLists.txt:  VERSION $version (so the tag must be $expected)"
  } >&2
  return 1
}

# notes_of <version>: the section of CHANGELOG.md for the version, on stdout.
notes_of() {
  local version=$1 file="$root/CHANGELOG.md"
  [ -n "$version" ] || usage "notes needs a version"
  if ! [[ $version =~ $version_pattern ]]; then
    echo "release-check: '$version' is not a version number such as 1.2.3 (a tag's v is not part of it)" >&2
    exit 2
  fi
  read_file "$file"
  case "$link_base" in
    '' | */) ;;
    *) link_base="$link_base/" ;;
  esac
  RELEASE_CHECK_VERSION="$version" RELEASE_CHECK_BASE="$link_base" awk '
    function fail(message) {
      print "release-check: " message > "/dev/stderr"
      exit 1
    }
    # absolute(target): a relative target made absolute, any other target as it is.
    function absolute(target,   angle) {
      angle = ""
      if (substr(target, 1, 1) == "<") {
        angle = "<"
        target = substr(target, 2)
      }
      if (target == "" || target ~ /^#/ || target ~ /^[A-Za-z][A-Za-z0-9+.-]*:/) {
        return angle target
      }
      sub(/^\//, "", target)
      return angle base target
    }
    # rewrite(text): every inline link target of the line made absolute.
    function rewrite(text,   out, at, rest, stop) {
      out = ""
      rest = text
      while ((at = index(rest, "](")) > 0) {
        out = out substr(rest, 1, at + 1)
        rest = substr(rest, at + 2)
        stop = index(rest, ")")
        if (stop == 0) {
          break
        }
        out = out absolute(substr(rest, 1, stop - 1))
        rest = substr(rest, stop)
      }
      return out rest
    }
    BEGIN {
      version = ENVIRON["RELEASE_CHECK_VERSION"]
      base = ENVIRON["RELEASE_CHECK_BASE"]
      prefix = "## [" version "]"
    }
    {
      line = $0
      sub(/\r$/, "", line)
      is_fence = (line ~ /^[ \t]*(```|~~~)/)
      if (is_fence) {
        fenced = !fenced
      }
      if (!is_fence && !fenced) {
        if (substr(line, 1, 3) == "## ") {
          in_section = 0
          if (substr(line, 1, length(prefix)) == prefix) {
            sections++
            in_section = (sections == 1)
          }
          next
        }
        if (line ~ /^\[[^]]+\]:[ \t]/) {
          in_section = 0
          next
        }
      }
      if (!in_section) {
        next
      }
      text = line
      if (base != "" && !is_fence && !fenced) {
        text = rewrite(line)
      }
      if (text ~ /^[ \t]*$/) {
        if (kept > 0) {
          blanks++
        }
        next
      }
      for (; blanks > 0; blanks--) {
        kept++
        out_line[kept] = ""
      }
      kept++
      out_line[kept] = text
    }
    END {
      if (sections == 0) {
        fail("CHANGELOG.md has no section for " version)
      }
      if (sections > 1) {
        fail("CHANGELOG.md has more than one section for " version)
      }
      if (kept == 0) {
        fail("CHANGELOG.md: the section for " version " is empty")
      }
      for (j = 1; j <= kept; j++) {
        print out_line[j]
      }
    }' "$file"
}

case "$command_name" in
  version)
    [ "$#" -eq 0 ] || usage "version takes no argument"
    project_version
    ;;
  check)
    [ "$#" -eq 1 ] || usage "check takes one tag"
    check_tag "$1"
    ;;
  notes)
    [ "$#" -eq 1 ] || usage "notes takes one version"
    notes_of "$1"
    ;;
  *) usage "no such command: $command_name" ;;
esac
