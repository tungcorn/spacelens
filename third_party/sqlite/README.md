# SQLite amalgamation (vendored)

Source: [SQLite amalgamation 3.53.4](https://www.sqlite.org/download.html)
(`sqlite-amalgamation-3530400`).

Files:

- `sqlite3.c` / `sqlite3.h` — amalgamation used by SpaceLens index storage
- `sqlite3ext.h` — extension API header (not required for the amalgamation build)

SpaceLens compiles `sqlite3.c` as C and links it into `spacelens_core`.
No network access is required at build time once these files are present.

Identifying macros in `sqlite3.h`:

```text
SQLITE_VERSION        "3.53.4"
SQLITE_VERSION_NUMBER 3053004
SQLITE_SOURCE_ID      "2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc"
```

The author of SQLite disclaims copyright to this source. In place of a
legal notice, `sqlite3.h` records this blessing:

```text
May you do good and not evil.
May you find forgiveness for yourself and forgive others.
May you share freely, never taking more than you give.
```

That blessing is SQLite's notice. It is not a SpaceLens project license
and is independent of Qt. Do not treat it as permission to distribute
SpaceLens or the Qt GUI runtime.
