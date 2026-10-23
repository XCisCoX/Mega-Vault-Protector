# Android icons (generated)

`drawable/` holds one Android `VectorDrawable` per Lucide glyph, generated from the
same SVG masters the desktop app uses:

```
python tools/icons/generate.py --android-only
```

They are staged here, outside `android/app/src/main/res/`, so the Android build is not
touched until it adopts the icon family. To adopt:

1. copy `drawable/*.xml` into `android/app/src/main/res/drawable/`
2. reference `R.drawable.ic_lucide_plus` (dashes become underscores)
3. tint at the call site — Compose:
   `Icon(painterResource(R.drawable.ic_lucide_plus), contentDescription = "Add",
   tint = MaterialTheme.colorScheme.onSurface)`

Each file is a `<vector>` with `viewportWidth/Height="24"`, `strokeWidth` from the
master, round caps and joins, and `pathData` converted from the SVG (`circle` and
`rect` are converted to equivalent paths, since VectorDrawable has no such elements).
Because they are vectors, there are no per-density bitmaps to maintain.

Licence: ISC, same as the masters — see `../lucide/LICENSE`.
