# SQLite amalgamation (vendored)

Source: [SQLite amalgamation 3.53.4](https://www.sqlite.org/download.html)
(`sqlite-amalgamation-3530400`).

Files:

- `sqlite3.c` / `sqlite3.h` — amalgamation used by SpaceLens index storage
- `sqlite3ext.h` — extension API header (not required for the amalgamation build)

SpaceLens compiles `sqlite3.c` as C and links it into `spacelens_core`.
No network access is required at build time once these files are present.
