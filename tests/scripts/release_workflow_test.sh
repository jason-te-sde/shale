#!/usr/bin/env bash
# Tests that the release workflow, RELEASING.md and the script they both rely on say the same thing.
#
# A workflow cannot be run here, and a release is the last place to find out that it was written
# with a different idea of the process than the page that describes it. So what can be held is held:
# the workflow runs on a tag and on nothing else, only the job that publishes can write, it runs the
# script that is tested in release_check_test.sh, it uses the checkout that CI uses, and the document
# names the files that the workflow produces. The checks read lines and do not parse YAML, which is
# why release.yml keeps the layout they expect.
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
    fail "$1: $2 does not contain '$3'"
  fi
}

# lacks <description> <file> <fixed text>: the file exists and does not contain the text.
lacks() {
  cases=$((cases + 1))
  if [ ! -f "$2" ]; then
    fail "$1: $2 does not exist"
  elif grep -qF -- "$3" "$2"; then
    fail "$1: $2 contains '$3'"
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

# The job that holds each 'contents: write', one name to a line.
writers="$(awk '
  /^jobs:/ { in_jobs = 1; next }
  in_jobs && /^  [A-Za-z0-9_-]+:[ \t]*$/ { job = $1; sub(/:$/, "", job) }
  /contents: write/ { print job }' "$workflow")"
cases=$((cases + 1))
if [ "$writers" != publish ]; then
  fail "'contents: write' must be held by the job named publish and by no other, found:" \
    "${writers:-no job}"
fi
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

# Once in each job: the publishing job does not trust the verdict of the one that tests.
cases=$((cases + 1))
checks="$(grep -cF 'scripts/release-check.sh check "$GITHUB_REF_NAME"' "$workflow")"
if [ "$checks" -ne 2 ]; then
  fail "the tag must be checked once in verify and once in publish, it is checked $checks time(s)"
fi
has "the notes come from the script" "$workflow" "scripts/release-check.sh"
has "the notes are cut from the changelog" "$workflow" 'notes "$VERSION" > "$RUNNER_TEMP/notes.md"'
has "the notes are the text of the release" "$workflow" '--notes-file "$RUNNER_TEMP/notes.md"'
has "the notes link to the tagged tree" "$workflow" "--link-base"
has "the build is a release build" "$workflow" "cmake --preset release"
has "the tests are run" "$workflow" "ctest --preset release"
has "the archive is made from the tag" "$workflow" "git archive --prefix="
has "the archive has a checksum" "$workflow" 'sha256sum "shale-$VERSION.tar.gz" > "shale-$VERSION.tar.gz.sha256"'
has "the checksum is checked" "$workflow" 'sha256sum --check "shale-$VERSION.tar.gz.sha256"'
has "both files are attached" "$workflow" '"shale-$VERSION.tar.gz" "shale-$VERSION.tar.gz.sha256"'
has "the release is created with gh" "$workflow" "gh release create"
has "the release is made from the tag that exists" "$workflow" "--verify-tag"

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
