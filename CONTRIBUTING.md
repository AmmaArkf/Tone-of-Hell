# Contributing

Thank you for wanting to help. This page explains how to report bugs, suggest ideas and plugins, and submit code.

## Contents

1. [Ground rule: only original, properly licensed work](#ground-rule-only-original-properly-licensed-work)
2. [Report a bug](#report-a-bug)
3. [Suggest an idea, an automation or a plugin](#suggest-an-idea-an-automation-or-a-plugin)
4. [How we handle issues](#how-we-handle-issues)
5. [Stabilization: bugs are fixed at the end of each level](#stabilization-bugs-are-fixed-at-the-end-of-each-level)
6. [Submit code](#submit-code)
7. [Real-time code rules](#real-time-code-rules)

## Ground rule: only original, properly licensed work

The project must stay fully open and free of legal risk. For that reason you **must not** add to issues or pull requests:

- code, user interface, icons, sounds, samples, loops or presets from other closed programs;
- code, file formats or algorithms obtained by disassembling, decompiling or reverse engineering closed programs;
- code or material whose license is incompatible with MIT or does not allow redistribution;
- screenshots and project files protected by someone else's copyright.

You may use well-known audio processing theory, open standards and documentation, and code under a compatible license (with the source and license stated). If you are unsure, ask in Discussions before you write any code.

With every pull request you confirm that you wrote the code yourself or have the right to submit it. To do so, sign off your commits: `git commit -s` (Developer Certificate of Origin).

## Report a bug

1. Search the Issues for a similar report. If you find one, add a 👍 and your details.
2. Open a new issue with the **"Report a bug"** template and fill in the fields.

The more precise the report, the faster the fix. We usually need:

- the program version and your operating system;
- your audio interface and driver (WASAPI, ASIO and so on), buffer size and sample rate;
- the steps that lead to the bug, starting from a clean project;
- what you expected and what actually happened;
- the error log, crash message or a screenshot;
- a minimal project that reproduces the bug (if you can share it without violating anyone's rights).

If the data contains anything private (file names, paths that include your user name), replace it before sending.

## Suggest an idea, an automation or a plugin

We want to hear what is missing. Use the templates:

- **"Suggest an idea or an automation"**: a new feature, a usability improvement, batch processing, macros and scripts.
- **"Suggest a plugin or an integration"**: a built-in effect or instrument, support for a plugin format, import or export of a file format.

We evaluate proposals with four questions:

1. **Value.** What problem does it solve, and for whom?
2. **License.** Can it be included in an MIT open source project, or connected as a separate plugin?
3. **Performance.** Will it disturb the real-time audio thread?
4. **Maintenance.** Who will maintain and test it?

Proposals with the most 👍 and a ready implementer go first. Proposals that require copying someone else's code or material are declined.

## How we handle issues

1. **Triage.** New issues get the `needs-triage` label. A maintainer checks that the description is complete, sets the type and priority, and asks for details if needed (`needs-info`).
2. **Discussion.** Ideas are discussed in the issue itself. The outcome: accept, postpone or decline, with an explanation.
3. **Implementation.** Accepted work gets the `help wanted` label or is assigned to someone.
4. **Closing.** An issue is closed automatically when a pull request with `Fixes #number` is merged into the main branch.

If an issue gets no reply for more than 30 days after we asked for details, it may be closed. It can always be reopened.

### Labels

| Label | Meaning |
| --- | --- |
| `bug` | A problem in existing behavior |
| `feature` | A new feature |
| `automation` | Scripts, macros, batch processing |
| `plugin` | An effect, an instrument or format support |
| `needs-triage` | Not reviewed yet |
| `needs-info` | Waiting for details from the author |
| `good first issue` | Suitable for a first contribution |
| `help wanted` | An implementer is needed |
| `duplicate` / `wontfix` | A duplicate / will not be done |
| `P0` to `P3` | Priority |

### Bug priorities

- **P0.** Loss or corruption of a project, a crash on startup, constant clicks or audio dropouts. Fixed first.
- **P1.** A main feature does not work and there is no workaround.
- **P2.** A feature works with limitations or there is a workaround.
- **P3.** Minor flaws, cosmetics, typos.

## Stabilization: bugs are fixed at the end of each level

Every major development level (see [ROADMAP.md](ROADMAP.md)) ends with a stabilization phase:

1. New features are frozen and a release candidate is published.
2. The community tests it and sends bug reports using the template.
3. Only fixes for P0 to P2 bugs and documentation changes are accepted.
4. When there are no open P0 or P1 bugs, the release is published.

Ideas and new plugins that arrive during stabilization are not lost: they go into the plan for the next level.

## Submit code

1. Find the issue you want to work on and say in it that you are taking it. For large changes, agree on a plan in the issue first.
2. Fork the repository and create a branch from the main one: `fix/short-description` or `feature/short-description`.
3. Write C++17 and follow the `.clang-format` file (it will appear together with the first code).
4. Add tests for new behavior. For a bug you fix, reproduce it with a test first.
5. Sign off your commits (`git commit -s`) and write clear messages: what changed and why.
6. Raise the third number of the version in the top-level `CMakeLists.txt` by one and describe your change at the top of `CHANGELOG.md` (see [docs/VERSIONING.md](docs/VERSIONING.md)).
7. Open a pull request using the template and link it to the issue (`Fixes #number`).
8. Respond to review comments. We try to reply quickly, but we cannot guarantee response times.

Small pull requests are merged faster than large ones. One task per pull request.

## Real-time code rules

The audio thread runs in real time, and mistakes in it are heard as clicks and dropouts. Code that runs on the audio thread **must not**:

- allocate or free memory (`new`, `delete`, `malloc`, growing a `std::vector` and the like);
- use blocking calls (mutexes, waiting, file reads and writes, network calls, console output);
- throw or catch exceptions;
- call code whose timing is unpredictable.

Communication with the user interface goes through lock-free queues or atomic variables. If you are not sure, ask in the pull request discussion.
