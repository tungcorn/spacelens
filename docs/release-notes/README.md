# Release notes

Each public version can have a notes file named after the Git tag:

```text
docs/release-notes/v<major>.<minor>.<patch>.md
```

The release workflow publishes that file as the GitHub Release body when it
exists. If the file is missing, the workflow uses a short generated fallback.
It never invents a one-line body when a notes file is present.

Edit `docs/release-notes/v0.1.0.md` and the existing GitHub Release body
together if the published notes need a correction. Do not retag `v0.1.0`.
