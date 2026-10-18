#!/usr/bin/env bash
# Tests that the release workflow, RELEASING.md and the script they both rely on say the same thing.
#
# A workflow cannot be run here, and a release is the last place to find out that it was written
# with a different idea of the process than the page that describes it. So what can be held is held:
# the workflow runs on a tag and on nothing else, only the job that publishes can write, it runs the
# script that is tested in release_check_test.sh, it uses the checkout that CI uses, and the document
# names the files that the workflow produces. Each check is there because the edit it forbids would
# leave ctest green and break the release, or publish one that verify had refused: a token dropped
# from the job that calls gh, a step that may fail without failing the job, a gate that moved, an
# archive of whatever the checkout holds. The checks read lines and do not parse YAML, which is why
# release.yml keeps the layout they expect.
# shellcheck disable=SC2016  # the texts searched for are workflow lines, and their $ is not ours
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=tests/scripts/lib.sh
. "$here/lib.sh"

source_dir="$(cd "$here/../.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
ci="$source_dir/.github/workflows/ci.yml"
releasing="$source_dir/RELEASING.md"
testing_guide="$source_dir/docs/testing.md"

# has <description> <file> <fixed text>: the file contains the text.
has() {
  cases=$((cases + 1))
  if ! grep -qF -- "$3" "$2" 2>/dev/null; then
    fail "$1: ${2##*/} does not contain '$3'"
  fi
}

# lacks <description> <file> <fixed text>: the file exists and does not contain the text.
lacks() {
  cases=$((cases + 1))
  if [ ! -f "$2" ]; then
    fail "$1: ${2##*/} does not exist"
  elif grep -qF -- "$3" "$2"; then
    fail "$1: ${2##*/} contains '$3'"
  fi
}

# lacks_line <description> <file> <extended regular expression>: no line of the file matches.
lacks_line() {
  cases=$((cases + 1))
  if grep -Eq -- "$3" "$2"; then
    fail "$1: ${2##*/} has a line that matches '$3'" "$(grep -En -- "$3" "$2")"
  fi
}

cases=$((cases + 1))
if [ ! -f "$source_dir/.github/workflows/release.yml" ]; then
  fail "there is no $source_dir/.github/workflows/release.yml"
  finish release_workflow_test
  exit
fi

# The workflow without its comment lines: a word that only a comment contains is not something the
# workflow does, and the comments say what the workflow does not do too.
workflow="$tmp/release.yml"
grep -Ev '^[[:space:]]*#' "$source_dir/.github/workflows/release.yml" >"$workflow"

# jobs_with <fixed text>: the jobs of the workflow that have a line containing the text, sorted, one
# name to a line.
jobs_with() {
  NEEDLE="$1" awk '
    /^jobs:/ { in_jobs = 1; next }
    in_jobs && /^  [A-Za-z0-9_-]+:[ \t]*$/ { job = $1; sub(/:$/, "", job) }
    in_jobs && index($0, ENVIRON["NEEDLE"]) > 0 { print job }' "$workflow" | sort -u
}

# in_jobs <description> <fixed text> <job>...: the text is in exactly these jobs, and so is not
# somewhere that would make it mean something else.
in_jobs() {
  local description=$1 needle=$2 found want
  shift 2
  want="$(printf '%s\n' "$@" | sort -u)"
  found="$(jobs_with "$needle")"
  cases=$((cases + 1))
  if [ "$found" != "$want" ]; then
    fail "$description: '$needle' is in ${found:-no job}, and should be in only ${want//$'\n'/ }"
  fi
}

# before <description> <first text> <second text>: the first line that has the first text comes before
# the first line that has the second.
before() {
  local first second
  first="$(grep -nFm1 -- "$2" "$workflow" | cut -d: -f1)"
  second="$(grep -nFm1 -- "$3" "$workflow" | cut -d: -f1)"
  cases=$((cases + 1))
  if [ -z "$first" ] || [ -z "$second" ] || [ "$first" -ge "$second" ]; then
    fail "$1: '$2' (line ${first:-none}) must come before '$3' (line ${second:-none})"
  fi
}

# --- When it runs. --------------------------------------------------------------------------------

cases=$((cases + 1))
if ! grep -Eq "^[[:space:]]+tags:[[:space:]]*\[[\"']v\*[\"']\][[:space:]]*\$" "$workflow"; then
  fail "the workflow does not run on a pushed tag matching v*"
fi
lacks "a release is made from a tag and from no branch" "$workflow" "branches:"
lacks "a release is not started by hand" "$workflow" "workflow_dispatch"
lacks "a release is not started by a pull request" "$workflow" "pull_request"

# --- What it may do. ------------------------------------------------------------------------------

cases=$((cases + 1))
top="$(awk '/^permissions:/ { getline; print; exit }' "$workflow")"
if [ "$top" != "  contents: read" ]; then
  fail "the permissions of the workflow as a whole are not 'contents: read'" "$top"
fi

in_jobs "only the job that publishes can write" 'contents: write' publish
lacks "no job takes every permission" "$workflow" "write-all"
has "the job that publishes waits for the one that tests" "$workflow" "needs: verify"

cases=$((cases + 1))
runners="$(grep -E '^[[:space:]]+runs-on:' "$workflow" | sed 's/^[[:space:]]*runs-on:[[:space:]]*//' | sort -u)"
if [ "$runners" != ubuntu-24.04 ]; then
  fail "every job must run on ubuntu-24.04, found:" "${runners:-no runs-on}"
fi

# --- What it runs. --------------------------------------------------------------------------------

cases=$((cases + 1))
in_workflow="$(grep -oE 'uses: actions/checkout@[A-Za-z0-9._-]+' "$workflow" | sort -u)"
in_ci="$(grep -oE 'uses: actions/checkout@[A-Za-z0-9._-]+' "$ci" | sort -u)"
if [ -z "$in_workflow" ]; then
  fail "the workflow does not check anything out"
elif [ "$in_workflow" != "$in_ci" ]; then
  fail "the checkout of the workflow is not the one CI uses" "workflow: $in_workflow
ci: $in_ci"
fi

# --- What each job does, and in which. ------------------------------------------------------------

# Nothing may go on past a failure, and nothing may be skipped: a job or a step that can fail without
# failing the job lets publish run after a red verify, and a condition can switch a gate off.
lacks "no job or step goes on after a failure" "$workflow" "continue-on-error"
lacks_line "no job or step is skipped by a condition" "$workflow" '^[[:space:]]*(-[[:space:]]+)?if:'

# The only expressions are the two values of the environment of publish. An expression in a command
# is pasted into the shell before it runs, and the name of a tag is not safe to paste.
cases=$((cases + 1))
expressions="$(grep -cF '${{' "$workflow")"
allowed="$(grep -cE '^[[:space:]]+(GH_TOKEN|GH_REPO):[[:space:]]+\$\{\{[[:space:]]*github\.(token|repository)[[:space:]]*\}\}[[:space:]]*$' "$workflow")"
if [ "$expressions" -ne "$allowed" ]; then
  fail "an expression is pasted somewhere other than the GH_TOKEN and GH_REPO of the environment" \
    "$(grep -nF '${{' "$workflow")"
fi
in_jobs "gh is given its token where it is called" 'GH_TOKEN: ${{ github.token }}' publish
in_jobs "gh is told the repository" 'GH_REPO: ${{ github.repository }}' publish
in_jobs "the write token is not left in the checkout" 'persist-credentials: false' publish

in_jobs "the tag is checked in both jobs" 'scripts/release-check.sh check "$GITHUB_REF_NAME"' publish verify
in_jobs "a version with no changelog entry is refused before the build" \
  'scripts/release-check.sh notes "$(scripts/release-check.sh version)"' verify
before "the tag is checked before anything is built" 'scripts/release-check.sh check "$GITHUB_REF_NAME"' \
  "cmake --preset release"
before "the changelog is checked before anything is built" \
  'scripts/release-check.sh notes "$(scripts/release-check.sh version)"' "cmake --preset release"
in_jobs "the build is a release build" "cmake --preset release" verify
in_jobs "the tests are run" "ctest --preset release" verify

# The archive is of the commit the run was triggered for, which verify tested and the checkout has
# shown to be the one the tag names. A branch or HEAD would be whatever the checkout happens to hold.
in_jobs "the archive is of the commit that was tested" \
  'git archive --prefix="shale-$VERSION/" -o "shale-$VERSION.tar.gz" "$GITHUB_SHA"' publish
in_jobs "the archive has a checksum" \
  'sha256sum "shale-$VERSION.tar.gz" > "shale-$VERSION.tar.gz.sha256"' publish
in_jobs "the checksum is checked" 'sha256sum --check "shale-$VERSION.tar.gz.sha256"' publish
in_jobs "the archive is unpacked and asked the question" \
  'release-check.sh --root "$RUNNER_TEMP/unpacked/shale-$VERSION" check "$GITHUB_REF_NAME"' publish
in_jobs "the notes are cut from the changelog" 'notes "$VERSION" > "$RUNNER_TEMP/notes.md"' publish
in_jobs "the notes link to the tagged tree" "--link-base" publish
in_jobs "the release is created with gh" "gh release create" publish
in_jobs "the release is made from the tag that exists" "--verify-tag" publish
in_jobs "the notes are the text of the release" '--notes-file "$RUNNER_TEMP/notes.md"' publish
in_jobs "both files are attached" '"shale-$VERSION.tar.gz" "shale-$VERSION.tar.gz.sha256"' publish

# --- What the document says. ----------------------------------------------------------------------

has "RELEASING.md names the workflow" "$releasing" ".github/workflows/release.yml"
has "RELEASING.md names the script" "$releasing" "scripts/release-check.sh"
has "RELEASING.md names the archive" "$releasing" "shale-X.Y.Z.tar.gz"
has "RELEASING.md names the checksum" "$releasing" "shale-X.Y.Z.tar.gz.sha256"
has "the workflow names the archive" "$workflow" 'shale-$VERSION.tar.gz'
has "the workflow names the checksum" "$workflow" 'shale-$VERSION.tar.gz.sha256'
has "the testing guide lists the script's test" "$testing_guide" "tests/scripts/release_check_test.sh"
has "the testing guide lists this test" "$testing_guide" "tests/scripts/release_workflow_test.sh"

finish release_workflow_test
