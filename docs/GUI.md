# SpaceLens GUI

Native Qt 6 Widgets shell. This is a desktop storage utility, not a website
and not a dashboard.

## Character

Dense, calm, technical, trustworthy. System UI font. System / Qt palette.
Accent is used only for the active workspace, the one primary command, and
selection.

## Information architecture

Two top-level workspaces — **Live Scan** and **Indexed** — in a compact
segmented selector. No menu bar, no permanent left application sidebar.
Cleanup Review and Maintenance History stay workflows, not destinations.
User-declared ordinary locations live under the navigation **More** overflow.

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

Wide windows use a ~70 / 30 table / inspector splitter. Results are a
Name / Size / Type / Class / Modified table. The right pane is a property
inspector (empty and “Unknown” fields are omitted) plus a Largest files
table. Metrics are a compact typographic strip, not cards. Advanced kind /
size / extension / class filters live in a Filters popup. A search box
filters the current listing only. Cancel is shown only while a scan is
running.

## Indexed

Resizable root list (~280–380 px) + selected-root exploration. Each root
shows a folder name, size · age, health, and a full-path tooltip. Discovery
modes are a segmented selector (Largest / Old & Large / Developer / Reclaim /
Custom). Search is wide; Sort stays visible; the rest of the criteria sit
behind Filters. The treemap sits above the result table; the inspector is
the right-hand pane. Empty, loading, and zero-result states replace a blank
table. The treemap follows the widget palette in both themes.

## Theme

No custom dark theme. Custom-painted widgets read `QPalette` roles.
Hard-coded light fills are forbidden on theme-dependent surfaces.

## Safety language

CLI and analysis remain read-only. The window status bar carries the
status message on the left and a compact **Read-only analysis** chip on
the right. Maintenance confirmation still says files go to
the Recycle Bin, this is not permanent deletion, Recycle Bin still occupies
disk space, and SpaceLens does not empty it. The primary confirmation button
remains **Move eligible files to Recycle Bin**.
