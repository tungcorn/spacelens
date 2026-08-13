# LICENSE_DECISION_REQUIRED

No project license has been chosen by a maintainer.

This file is a status marker, not a license. It does not grant rights to use,
copy, modify, or distribute SpaceLens or the staged zip archives.

Until a maintainer writes a real `LICENSE` file at the repository root:

- do not treat source or binaries as publicly redistributable
- GitHub Release publish stays skipped (`cli_eligible` is false)
- overall distribution status remains `RELEASE_DISTRIBUTION_BLOCKED`

A future `LICENSE` can unlock a CLI-only GitHub Release. It does not
unlock the GUI zip. The GUI also needs a structured Qt review PASS with
maintainer-controlled corresponding source. See `docs/QT_REDIST_AUDIT.md`.

An AI assistant must not choose a license.
