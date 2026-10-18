# Releasing shale

A release is a tag. The tag is what users build from, so the checks that matter run before it is pushed
and again, by a workflow, on the commit it names.

## Versions

shale uses [Semantic Versioning](https://semver.org/), with two things to know.

- **Before 1.0, a minor release may change the API.** `0.2.0` may not be a drop-in for `0.1.x`. The CMake
  package's version file accepts any installed version with the same *major* number that is at least the
  one asked for (`SameMajorVersion`), so `find_package(shale 0.1)` is satisfied by an installed `0.1.0`
  and refused by an installed `1.0`, and it is also satisfied by an installed `0.2`. A project that needs
  a particular `0.x` should check `shale_VERSION` itself.
- **The on-disk format has its own version**: the table format version in the footer
  ([docs/format.md](docs/format.md#compatibility)). A release that changes any file the engine writes
  says so at the top of its changelog entry, and a change to the table layout bumps that version, so an
  older reader refuses a newer table and does not misread it.

The version is written in one place, `project(shale VERSION x.y.z ...)` in `CMakeLists.txt`.

## To make a release

1. **Check that `main` is green**: every job of CI on the commit you mean to release, and
   `scripts/preflight.sh` on your machine if you have one that can run it.
2. **Write the changelog.** In [CHANGELOG.md](CHANGELOG.md) rename `[Unreleased]` to the new version
   with today's date, start a new empty `[Unreleased]`, and fix the compare links at the bottom. List
   what a user of the library, the tools or the files needs to know: changes of behaviour first, then
   additions, then fixes. The heading is `## [X.Y.Z] - YYYY-MM-DD`: the section under it, up to the
   next `## ` heading or to the link definitions at the bottom, is the text of the release page, so
   write its links inline. The workflow makes a relative link absolute; it does not carry a
   `[text][name]` reference's definition over.
3. **Set the version.** Edit `VERSION` in `CMakeLists.txt` to match. Commit both changes together with
   the subject `chore(build): release x.y.z`, through a pull request like any other change.
4. **Tag the merged commit** and push the tag. The tag is `v` and the version, annotated. The first two
   commands ask the questions that the workflow asks, so that a mistake is found here and not on the
   Actions page: the first says whether the tag is the version of `CMakeLists.txt`, and the second prints
   what the release page will say.

   ```
   git switch main && git pull
   scripts/release-check.sh check vX.Y.Z
   scripts/release-check.sh notes X.Y.Z
   git tag -a vX.Y.Z -m "shale X.Y.Z"
   git push origin vX.Y.Z
   ```

5. **The release workflow** (`.github/workflows/release.yml`) runs on a pushed `v*` tag, in two jobs.
   - `verify` cannot write to the repository. It **refuses** a tag that does not equal `v` plus the
     `VERSION` in `CMakeLists.txt` (step 4's check, which prints both versions), so a tag can never name
     a version the source does not say it is, and a version that the changelog has no entry for. It
     then builds Release on Ubuntu 24.04 and runs the tests: `cmake --preset release`,
     `cmake --build --preset release` and `ctest --preset release`.
   - `publish` runs only when `verify` passed, and is the only job with write access to the repository
     (`contents: write`). It checks the tag again and makes `git archive --prefix=shale-X.Y.Z/` of the
     tag, as `shale-X.Y.Z.tar.gz`, and its checksum as `shale-X.Y.Z.tar.gz.sha256`, in the format
     `sha256sum --check` reads. It unpacks the archive and asks the same question of its
     `CMakeLists.txt`, and then creates the GitHub release with both files attached and the changelog
     entry as its notes, its links pointing at the tagged tree.
6. **Look at the release.** Download the source archive and its checksum and check one against the other
   (`sha256sum --check shale-X.Y.Z.tar.gz.sha256`, or `shasum -a 256 --check` on macOS), then build and
   test the archive from a clean directory (`cmake --preset release`, `cmake --build --preset release`,
   `ctest --preset release`), then install it and build `examples/` against the installation as the
   `clean-clone` job does. The workflow tests the tagged commit and not the unpacked archive, so this is
   the step that tests the archive.

## If something goes wrong

- **The workflow refused the tag, or `verify` failed**: the tag does not match the version, the changelog
  has no entry for it, or a test failed. Nothing was published, because `publish` waits for `verify`.
  Delete the tag (`git push origin :refs/tags/vX.Y.Z` and `git tag -d vX.Y.Z`), fix the cause through a
  pull request, and tag the new commit.
- **`publish` failed, and nothing in the source is to blame**: a runner or the network. Re-run the failed
  job from the Actions page. If it stopped after it had made the release page, delete that page (not the
  tag) first: `gh release create` refuses to replace a release, and the page that is left holds fewer
  files than the release should.
- **A published release is bad**: do not move or delete its tag, since people may have built from it.
  Release a patch version that fixes it and say so in the changelog. If the problem is a vulnerability,
  see [SECURITY.md](SECURITY.md).
- **A fix for a released version** goes to `main` first and is released from there; there are no release
  branches while only one release is supported.
