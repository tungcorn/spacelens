# SpaceLens GUI

Native Qt 6 Widgets shell. This is a desktop storage utility, not a website
and not a dashboard.

## Character

Dense, calm, technical, trustworthy. System UI font. System / Qt palette.
Accent is used only for the active workspace, the one primary command, and
selection.

## Information architecture

Two top-level workspaces — **Live Scan** and **Indexed** — in a segmented
navigation strip. No permanent left application sidebar. Cleanup Review and
Maintenance History stay workflows, not destinations.

Common page shell:

```text
workspace navigation
    ↓
page header (title + one-line purpose + commands)
    ↓
contextual controls
    ↓
content
```

Page padding is 16–20 logical px. Spacing uses 4 / 8 / 12 / 16 / 24.

## Command hierarchy

- **Primary** — one per state (Scan, Index Folder, Move eligible files to
  Recycle Bin). Application highlight color.
- **Secondary** — Choose Folder, Refresh, Show in Explorer.
- **Tertiary** — Rebuild, Copy Path, Reload List, behind a More menu.

Safety-critical actions are never hidden in an overflow menu.

## Live Scan

Choose a folder → scan → inspect results → optionally add to Cleanup Review.

Wide windows use a ~70 / 30 list / details splitter. Metrics are a compact
typographic strip, not cards. Advanced kind / size / extension / class
filters live in a Filters popup. A search box filters the current listing
only.

## Indexed

Resizable root list (~260–340 px) + selected-root exploration. Each root
shows path, size · age, and a textual health state. Discovery modes are a
segmented selector, not a row of unrelated buttons. Search is wide; Sort
stays visible; the rest of the criteria sit behind Filters. The treemap is
first-class content and follows the widget palette in both themes.

## Theme

No custom dark theme. Custom-painted widgets read `QPalette` roles.
Hard-coded light fills are forbidden on theme-dependent surfaces.

## Safety language

CLI and analysis remain read-only. The window status bar carries a compact
**Read-only analysis** cue. Maintenance confirmation still says files go to
the Recycle Bin, this is not permanent deletion, Recycle Bin still occupies
disk space, and SpaceLens does not empty it. The primary confirmation button
remains **Move eligible files to Recycle Bin**.
