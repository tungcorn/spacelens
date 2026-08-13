# Release notes

Each public version can have a notes file named after the Git tag:

```text
docs/release-notes/v<major>.<minor>.<patch>.md
```

The release workflow publishes that file as the GitHub Release body when it
exists. If the file is missing, the workflow uses a short generated fallback.
It never invents a one-line body when a notes file is present.

Edit a published notes file and the matching GitHub Release body
together if the notes need a correction. Do not retag a published `v*`
tag. Do not replace published zip binaries.
