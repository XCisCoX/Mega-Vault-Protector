# Icons

## The family: Lucide

Everything in the desktop app draws from [Lucide](https://lucide.dev), licensed
**ISC** — permissive, so it combines with this repository's AGPL-3.0 without adding
conditions. The licence text travels with the assets in
`assets/icons/lucide/LICENSE`.

Why Lucide over the alternatives that were measured (Tabler, Phosphor, Heroicons,
Feather, Iconoir, Material Symbols):

- complete coverage of every action the app needs — 40 glyphs, no substitutions
- a 24px grid with a 2px stroke, which matches the app's thin-stroke look and stays
  legible down to 16px
- Qt 5.12 renders it as-is: the icons only use `path`, `circle` and `rect`, and
  `stroke="currentColor"` parses correctly

## One source of truth, generated consumers

```
assets/icons/lucide/*.svg            masters (committed, the only hand-edited form)
        |
        +-- tools/icons/generate.py --+
                                      |
   qt-app/resources/icons/lucide/*.png   assets/icons/android/drawable/*.xml
   + qt-app/resources/icons.qrc          (VectorDrawables)
```

`icons.qrc` is generated too — adding an icon never means editing a resource list.

The PNGs are **committed on purpose**. The Linux CI installs `qtbase5-dev`, which
does not include QtSvg; loading SVG at runtime would add a package to every job for a
cosmetic gain, and the retina ladder is 68 KiB in total. The helper's API would not
change if we later switch to SVG (`libqt5svg5-dev` + `Qt5::Svg`).

Regenerating (only needed when the masters change — CI never runs this):

```
python tools/icons/generate.py --rasteriser out/build/tools/svg2png.exe
```

`svg2png` is `tools/icons/svg2png.cpp`, built with the same Qt kit; the Qt `bin`
directory must be on `PATH`. `rsvg-convert` works too if it is installed.

To add one icon: drop `assets/icons/lucide/<name>.svg` in, run the generator, and use
`icons::lucide("<name>")`. Nothing else to register.

## Using them in the Qt app

`qt-app/include/videovault/app/icons.hpp` is the only entry point:

- `icons::lucide(name)` — tinted from the live palette
- `icons::lucideSolid(name, colour)` — one colour for every state; for glyphs that sit
  on a coloured fill (white on the accent button)
- `icons::apply(button, name, tone)` — puts an icon on a button and re-tints it on
  `QApplication::paletteChanged`, so a theme switch needs no call-site code

Two tones keep the visual hierarchy of the old text buttons: `Tone::Normal` for chrome,
`Tone::Quiet` for row actions, which sit back at rest and come up to full strength on
hover (Qt asks the icon for its `Active` pixmap, so no event filter is needed).

Rules that go with icon-only buttons:

- always set a tooltip **and** an accessible name — the tooltip is the label
- destructive or primary dialog actions keep their words and gain an icon; the glyph
  alone is not enough for "Delete permanently" or "Create a new vault"
- row actions are hidden rather than faded: an icon does not follow the text colour,
  and an invisible button you can still click is a trap. The action column keeps its
  width and rows keep a minimum height, so nothing shifts when they appear

## Other platforms

**Windows and Linux (Qt)** — exactly the pipeline above. No QtSvg is needed at
build or run time, so the existing CI jobs and the vcpkg preset stay as they are; the
only new inputs are `assets/icons/lucide/*.svg` and the generated `icons.qrc`.

**Android** (native Kotlin/Compose, not Qt). `assets/icons/android/drawable/` holds a
generated `<vector>` per icon with the same path data, stroke width and round caps.
They live outside `res/` on purpose so the Android build is untouched until it adopts
them; copy them into `app/src/main/res/drawable/` and use
`Icon(painterResource(R.drawable.ic_lucide_plus), contentDescription = ...)`; tint with
the Compose `tint` argument or `android:tint` on an `ImageView`. Vector drawables scale
to any density, so no per-dpi bitmaps are needed.

**macOS / iOS later.** Two workable routes, both fed by the same masters:

1. Export the SVGs to single-colour PDF template images and use them as template
   images (SwiftUI `Image(...).renderingMode(.template)`), which keeps one icon family
   on every platform — recommended for the app's own actions, since the app should look
   like itself everywhere.
2. SF Symbols for native feel, with a documented name mapping (Lucide `chevron-right`
   ↔ `chevron.right`, and so on). Use this only for OS-standard chrome if native
   integration matters more than brand consistency.

Either way the generator gains an exporter; no platform changes what the masters are.

## Coverage

Converted to Lucide (visually verified offscreen, both themes):

- record editor: header actions, card actions, row actions, disclosure chevrons
- in-window sheets: Cancel / Save / Delete / Rename / Create / Move / Open / Discard /
  OK / Close, by label
- main-window chrome: add, share, settings, lock, check-for-update, the three view
  modes, the folder-pane toggle

Not yet converted: the remaining labelled buttons in the older dialogs and secondary
windows (`Unlock vault`, `Create vault`, `Sync now`, `Transfers`, `Pop out`, `Fit`,
`Browse…`, and the rest). They are plain `QPushButton`s with app-authored labels; the
intent is icon-only for compact actions and icon+text for primary/destructive ones.
They were left alone rather than converted blind, because none of those dialogs can be
rendered on this host without a human, and a wrong glyph there costs more than the
inconsistency does.
