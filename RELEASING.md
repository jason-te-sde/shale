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
   additions, then fixes.
3. **Set the version.** Edit `VERSION` in `CMakeLists.txt` to match. Commit both changes together with
   the subject `chore(build): release x.y.z`, through a pull request like any other change.
4. **Tag the merged commit** and push the tag. The tag is `v` and the version, annotated:

   ```
   git switch main && git pull
   git tag -a vX.Y.Z -m "shale X.Y.Z"
   git push origin vX.Y.Z
   ```

5. **The release workflow** (`.github/workflows/release.yml`) runs on a pushed `v*` tag. It
   - **refuses** a tag that does not equal `v` plus the `VERSION` in `CMakeLists.txt`, so a tag can never
     name a version the source does not say it is;
   - builds and runs the tests;
   - makes a source archive of the tagged tree and its `sha256`;
   - creates the GitHub release with both attached, and the changelog entry as its notes.
6. **Look at the release.** Download the source archive, check it against its `sha256`, and build and
   test it from a clean directory (`cmake --preset release`, `cmake --build --preset release`,
   `ctest --preset release`), then install it and build `examples/` against the installation as the
   `clean-clone` job does.

## If something goes wrong

- **The tag does not match the version**: delete the tag (`git push origin :refs/tags/vX.Y.Z` and
  `git tag -d vX.Y.Z`), fix `CMakeLists.txt`, and tag again. Nothing was published, because the workflow
  refused it.
- **A published release is bad**: do not move or delete its tag, since people may have built from it.
  Release a patch version that fixes it and say so in the changelog. If the problem is a vulnerability,
  see [SECURITY.md](SECURITY.md).
- **A fix for a released version** goes to `main` first and is released from there; there are no release
  branches while only one release is supported.
