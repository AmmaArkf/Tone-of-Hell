# Security

## How to report a vulnerability

Do not open a public issue about a vulnerability. Use GitHub's private channel:

1. Open the **Security** tab of the repository.
2. Click **Report a vulnerability**.
3. Describe the problem: what happens, how to reproduce it and what harm is possible.

We will reply as soon as we can, assess the severity and prepare a fix before the details are published. We cannot guarantee a response time, but we aim to reply within a week.

## What counts as a vulnerability

For a DAW this is primarily:

- errors when reading files (audio, MIDI, projects, presets) that lead to a crash, memory corruption or execution of foreign code;
- errors in plugin loading and in handling plugin data that can be abused;
- unsafe handling of file paths and temporary files;
- any network features (if they appear) that expose user data.

Ordinary crashes and bugs that pose no security risk should be reported with the "Report a bug" template.

## Supported versions

Fixes are released for the latest version only. Older versions are not supported.
