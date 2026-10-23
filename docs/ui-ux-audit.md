# UI/UX audit — Windows app (pass 1)

Audited on the real running app (MinGW fallback build: music + PDF windows are
stubs here, so those two surfaces cannot be judged on this build). Each finding
below was seen on screen, not inferred from code.

## Findings (seen)

| # | Where | Problem | Severity |
|---|---|---|---|
| F1 | Status bar | Shows the full vault filesystem path (`Vault location: C:/Users/...`). Leaks where the encrypted vault lives in every screenshot and on a shared screen. Explorer shows only the item name. | High (privacy) |
| F2 | List view | Item icons render ~24 px and unreadably small even though list mode sets a 44 px icon. PNG thumbnails are unrecognisable smudges; the video shows a flat orange square instead of a frame or a clear type badge. | High |
| F3 | All views | Sizes under ~10 KB print as `0.00 MB`. A 4 KB video reads "0.00 MB", which looks like an empty file. Units must step down to KB. | High |
| F4 | List view | Rows are 62 px tall with the name on the first line and the size on a second line far below. A list view should be one row per item: icon, name, size, date, aligned. | Medium |
| F5 | List view | The parent row is labelled `..` with a folder icon and takes part in selection/drag like a real item. Explorer uses an explicit "Up" affordance and never makes `..` a draggable item. | Medium |
| F6 | Toolbar | Six unlabelled glyph buttons (download-arrow, plus, share, sync, settings, lock). Import is split across a download-shaped arrow and a plus; nothing says which does what, and no tooltips appear. | Medium |
| F7 | Toolbar | The sort control ("↑ Name") only exists in the icon/list views and disappears in Details, where sorting moves to column headers. Same operation, two different places, one of them with an unexplained arrow glyph. | Medium |
| F8 | Toolbar | The "All items" dropdown sits beside the view-mode buttons with no label; with a folder open in the breadcrumb it is unclear whether it filters the folder or the whole vault. | Medium |
| F9 | Gallery | Empty space has no drop hint and no empty-state text. Dropping into it picks the current folder, but nothing on screen says so. | Low |
| F10 | Folder pane | Collapse control is a 20 px chevron at the pane's far edge — below the 24 px minimum target and easy to miss. "Folders" header consumes a full row for one word. | Medium |
| F11 | Header | The page title says "Library" and the breadcrumb directly beneath repeats "Library". | Low |
| F12 | Whole app | Secondary text (sizes, "Folder" type labels, status bar) is very low contrast on black — well under WCAG AA for small text. | High (a11y) |
| F13 | Gallery | Right-clicking a row moved the selection and did not show a context menu at the pointer; needs confirmation on item rows vs folder rows. | High (if confirmed) |
| F14 | List/icon views | No duration, resolution or codec information outside Details view, even though probing data exists. | Low |
| F15 | Gallery | With `h264-unmarked-key.mp4` visibly selected, pressing Enter opened **README.md** instead. The open action follows a stale current item instead of the selection — same family of bug as the drop using the selection instead of the dragged rows. | Critical (wrong item) |
| F16 | Text viewer | An imported Markdown file opens in an editable-looking surface with an always-enabled primary **Save** button and no dirty/read-only indication. A user cannot tell whether they are about to modify the vault copy. | High |
| F17 | Text viewer | Header hierarchy is inverted: "Back" is a small text link while "Save" is a large filled primary button, so the destructive-ish action outweighs the navigation one. | Medium |
| F18 | Text viewer | No find, no wrap toggle, no font-size control for long documents; the file name is only a small label next to Back, with no window title. | Medium |
| F19 | Media open | When a media file does not open, nothing on screen says what failed — the previous surface simply stays or another item opens. No error/loading state. | High |

## Not yet visually audited (next passes)

Surfaces enumerated from the code; every one still needs a look:

- Login/unlock, create-vault, sync (`sync_page`), settings (`settings_dialog`).
- Details view header, sorting, column resizing, tag column.
- Import menu (files/folder/note/record), drag-import from Explorer, progress/transfer panel, batch cancel.
- Dialogs: New folder, Rename (file/folder/tag), Remove/Remove items/Remove folder, Edit tags, New note, Save note, Change vault password, PDF password, Share with phone (QR), Record window.
- Media surfaces: video player (controls, seek, volume, fullscreen, next/prev), image viewer (zoom/pan/fit), text/record viewer, music window (stub here — needs the MSVC build), PDF window (stub here — needs the MSVC build).
- Error, empty and busy states; keyboard focus order; DPI scaling; long names and deep paths (truncation with ellipsis?); mixed-case and non-ASCII names; thousands of items.

## Fix plan

Ordered so visible damage is fixed first and each phase ends with a re-test.

**Phase 1 — privacy, units, contrast (F1, F3, F12)**
- Replace the status-bar path with the vault's display name; keep the path in Settings only.
- Central size formatter: B/KB/MB/GB with one decimal, no "0.00 MB" for a non-empty file.
- Raise secondary text to a contrast-checked token (measure against the background; target ≥4.5:1 for small text).
- Re-test: every size on screen, status bar, and a contrast measurement per text token.

**Phase 2 — list/icon views (F2, F4, F5, F14)**
- List mode: single-line rows, 44 px icons actually applied, name/size/date columns aligned.
- Generate/attach thumbnails for images and video frames in list and icon views; fall back to a type badge, never a flat colour square.
- Replace the `..` row with an "Up" button/row that is not draggable and not part of multi-select.
- Show duration in list mode for media.
- Re-test: same four files in all three views, plus a 1,000-item folder for scrolling cost.

**Phase 3 — toolbar and chrome (F6, F7, F8, F9, F10, F11)**
- Label the primary actions (Import, Share, Sync) and give every icon button a tooltip and accessible name.
- One sorting affordance per view; explain the arrow glyph.
- Give the filter dropdown a visible label and make "All items" vs the open folder unambiguous.
- Empty-state text in the gallery that also states files can be dropped here.
- Enlarge the pane toggle to ≥24 px; drop the redundant "Library" title or the duplicate breadcrumb.
- Re-test: keyboard-only pass through the toolbar (Tab order, focus ring, Enter/Space activation).

**Phase 4 — interaction correctness (F13 + drag/drop)**
- Context menu on right-click for item rows, folder rows and empty space, opening at the pointer without changing selection beyond the clicked row.
- Verify drag/drop in all three views and the folder pane after the hit-test fix.
- Re-test: every menu entry, plus drag onto pane/tree/breadcrumb/empty space and the folder-into-descendant refusal.

**Phase 5 — remaining dialogs and media windows**
- Open each dialog listed above, capture it, and log layout/spacing/contrast/label/target-size problems before fixing.
- Music and PDF windows require the MSVC + C++/WinRT build; audit them from a CI build artefact, not the fallback.

**Phase 6 — full re-test**
- Walk the whole surface list again with a checklist, run the headless suites (14/14 native, 17/17 relay), and record clicked-versus-compiled for every row.
