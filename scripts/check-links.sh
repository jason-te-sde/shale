#!/usr/bin/env bash
# scripts/check-links.sh [root]
#
# Checks that every relative link in the Markdown files under `root` (default: the repository root)
# points at something that exists, and, when the link has a #fragment and the target is a Markdown
# file, that the file has a heading that the fragment names. It reads the files and nothing else: a
# link to another site is not followed, since a check that goes to the network fails for reasons
# that are not in the repository.
#
# What counts as a link:
#   [text](target)  and  ![alt](target)     inline links and images; a "title" after the target is
#                                           ignored, and so are <angle brackets> around it
#   [name]: target                          a reference definition
#   href="target"  and  src="target"        the same in raw HTML
# A target with a scheme (https:, mailto:, ...) is external and is skipped. A target that starts with
# a slash is taken from `root`, as GitHub does. Links inside a fenced code block and inside an inline
# code span are examples, not links, and are skipped.
#
# A fragment is matched against the headings of the target file the way GitHub names them: the
# heading in lower case, every character that is not a letter, digit, space, hyphen or underscore
# removed, spaces turned into hyphens, and a heading that repeats an earlier one's name given -1, -2
# and so on. An id= or name= attribute in the file names a fragment too.
#
# The files checked are every *.md under `root` except those under .git, build and _deps. A broken
# link is printed as `path:line: broken link: target` (with the reason after it).
#
# Exit status: 0 every link resolves, 1 a link is broken, 2 nothing to check. A checker that checked
# nothing must not say "ok".
set -uo pipefail
export LC_ALL=C

root="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$root" 2>/dev/null || {
  echo "check-links: cannot enter $root" >&2
  exit 2
}

files=()
while IFS= read -r -d '' file; do
  files+=("${file#./}")
done < <(find . \( -name .git -o -name build -o -name _deps \) -prune -o -type f -name '*.md' -print0 | sort -z)
if [ "${#files[@]}" -eq 0 ]; then
  echo "check-links: no Markdown file under $root, so there is nothing to check" >&2
  exit 2
fi

# The links of a file, one per line as `line number<TAB>target`.
extract_links() {
  awk '
    function without_code_spans(s,   out, a, b) {
      out = ""
      while ((a = index(s, "`")) > 0) {
        out = out substr(s, 1, a - 1)
        s = substr(s, a + 1)
        b = index(s, "`")
        if (b == 0) {
          return out "`" s
        }
        s = substr(s, b + 1)
      }
      return out s
    }
    function emit(target) {
      sub(/^</, "", target)
      sub(/>$/, "", target)
      sub(/[ \t].*$/, "", target)
      sub(/^</, "", target)
      sub(/>$/, "", target)
      if (target != "") {
        printf "%d\t%s\n", NR, target
      }
    }
    /^[ \t]*(```|~~~)/ { fenced = !fenced; next }
    fenced { next }
    {
      line = without_code_spans($0)
      rest = line
      while ((p = index(rest, "](")) > 0) {
        rest = substr(rest, p + 2)
        q = index(rest, ")")
        if (q == 0) {
          break
        }
        emit(substr(rest, 1, q - 1))
        rest = substr(rest, q + 1)
      }
      if (line ~ /^ ? ? ?\[[^]]+\]:[ \t]+/) {
        t = line
        sub(/^ ? ? ?\[[^]]+\]:[ \t]+/, "", t)
        emit(t)
      }
      rest = line
      while (match(rest, /(href|src)="[^"]*"/)) {
        t = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        sub(/^(href|src)="/, "", t)
        sub(/"$/, "", t)
        emit(t)
      }
    }' "$1"
}

# The fragments a Markdown file can be linked to, one per line.
list_fragments() {
  awk '
    /^[ \t]*(```|~~~)/ { fenced = !fenced; next }
    fenced { next }
    /^#+[ \t]+/ {
      text = $0
      sub(/^#+[ \t]+/, "", text)
      sub(/[ \t]+#+[ \t]*$/, "", text)
      slug = tolower(text)
      gsub(/[^a-z0-9 _-]/, "", slug)
      gsub(/ /, "-", slug)
      count = seen[slug]++
      print (count == 0 ? slug : slug "-" count)
    }
    {
      rest = $0
      while (match(rest, /(id|name)="[^"]*"/)) {
        t = substr(rest, RSTART, RLENGTH)
        rest = substr(rest, RSTART + RLENGTH)
        sub(/^(id|name)="/, "", t)
        sub(/"$/, "", t)
        print t
      }
    }' "$1"
}

broken=0
checked=0
report() { # <file> <line> <target> <reason>
  printf '%s:%s: broken link: %s (%s)\n' "$1" "$2" "$3" "$4"
  broken=$((broken + 1))
}

# has_fragment <file> <fragment>: whether <file> has a heading or an id that the fragment names.
has_fragment() {
  list_fragments "$1" | grep -qxF -- "$2"
}

for file in "${files[@]}"; do
  dir="$(dirname "$file")"
  while IFS=$'\t' read -r line target; do
    [ -n "$target" ] || continue
    # A scheme (https:, mailto:, ...) makes a target external, and it is not followed.
    if [[ "$target" =~ ^[A-Za-z][A-Za-z0-9+.-]*: ]]; then
      continue
    fi
    checked=$((checked + 1))

    path="${target%%#*}"
    fragment=""
    case "$target" in
      *'#'*) fragment="${target#*#}" ;;
    esac
    path="${path%%\?*}"

    if [ -z "$path" ]; then
      resolved="$file"
    elif [ "${path#/}" != "$path" ]; then
      resolved=".$path"
    else
      resolved="$dir/$path"
    fi

    if [ ! -e "$resolved" ]; then
      report "$file" "$line" "$target" "no such file"
      continue
    fi
    if [ -n "$fragment" ]; then
      case "$resolved" in
        *.md)
          if [ -f "$resolved" ] && ! has_fragment "$resolved" "$fragment"; then
            report "$file" "$line" "$target" "no heading or id named #$fragment in $resolved"
          fi
          ;;
      esac
    fi
  done < <(extract_links "$file")
done

if [ "$broken" -ne 0 ]; then
  echo "check-links: $broken broken link(s) among $checked relative link(s) in ${#files[@]} file(s)" >&2
  exit 1
fi
if [ "$checked" -eq 0 ]; then
  echo "check-links: ${#files[@]} Markdown file(s) hold no relative link, so nothing was checked" >&2
  exit 2
fi
echo "check-links: ok, $checked relative link(s) in ${#files[@]} file(s)"
