# Version numbers

A version has three numbers: `release.prerelease.change`, for example `1.2.15`.

| Number | When it goes up | Example |
| --- | --- | --- |
| Third (change) | With every change that is committed to the main branch | `0.0.4` to `0.0.5` |
| Second (pre-release) | When a pre-release is published for testing; the third number returns to 0 | `0.0.9` to `0.1.0` |
| First (release) | When a full, stable release is published; the second and third numbers return to 0 | `0.3.2` to `1.0.0` |

Rules for contributors:

- The version is set in one place: `project(ToneOfHell VERSION ...)` in the top-level `CMakeLists.txt`. The program shows it in its status bar and in "Help > About", together with the commit it was built from.
- Every pull request raises the third number by one and adds an entry for the new version at the top of [CHANGELOG.md](../CHANGELOG.md).
- Only maintainers raise the first and second numbers, when they publish a release. A release gets a Git tag with the same number, for example `v0.1.0`.
