#!/usr/bin/env python3
"""Derive every icon and icon-adjacent resource from the two brand masters.

docs/branding/app-icon.png      the application mark (Windows, Linux, Android,
                                README)
docs/branding/package-icon.png  the mark used for .mvpk export packages

The masters are transparent PNGs. This script only ever downscales them with
LANCZOS on RGBA, so the transparent margin around the artwork is preserved and
nothing is composited onto a background. The single exception is the Android
adaptive-icon background colour, which is sampled from the dark plate inside
the master itself so the launcher mask reveals the same picture.

Requires Pillow. Run from anywhere:

    python scripts/generate-icons.py

The Windows .ico files carry 16-256 px frames so Explorer stops blurring the
taskbar and the large-icon view.
"""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image

RESAMPLE = getattr(Image, "Resampling", Image).LANCZOS

ROOT = Path(__file__).resolve().parents[1]
BRAND = ROOT / "docs" / "branding"
APP_MASTER = BRAND / "app-icon.png"
PACKAGE_MASTER = BRAND / "package-icon.png"

ICO_SIZES = (16, 24, 32, 48, 64, 128, 256)
ANDROID_DENSITIES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}
ADAPTIVE_FOREGROUND_PX = 432  # 108 dp at xxhdpi
HICOLOR_SIZES = (16, 24, 32, 48, 64, 128, 256, 512)

QT_RES = ROOT / "qt-app" / "resources"
QT_ANDROID_RES = ROOT / "qt-app" / "android" / "res"
ANDROID_RES = ROOT / "android" / "app" / "src" / "main" / "res"
LINUX_RES = QT_RES / "linux"


def load(path: Path) -> Image.Image:
    return Image.open(path).convert("RGBA")


def scaled(image: Image.Image, size: int) -> Image.Image:
    if image.size == (size, size):
        return image.copy()
    return image.resize((size, size), RESAMPLE)


def write_png(image: Image.Image, path: Path, size: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    scaled(image, size).save(path, format="PNG", optimize=True)
    print(f"{path.relative_to(ROOT)}  {size}x{size}")


def write_ico(image: Image.Image, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    # Pillow stores every requested size as its own frame; 128 and 256 are
    # written as PNG-compressed frames, the smaller ones as bitmaps.
    image.resize((ICO_SIZES[-1], ICO_SIZES[-1]), RESAMPLE).save(
        path, format="ICO", sizes=[(size, size) for size in ICO_SIZES]
    )
    with Image.open(path) as check:
        ico = getattr(check, "ico", None)
        frames = sorted(ico.sizes()) if ico is not None else []
    print(f"{path.relative_to(ROOT)}  {', '.join(f'{w}x{h}' for w, h in frames)}")


def plate_colour(image: Image.Image) -> str:
    """Median colour of the dark plate inside the mark, as #rrggbb.

    Sampled on an annulus between the artwork and its rim, ignoring the bright
    lettering, so the Android launcher background continues the plate instead
    of framing it in a colour the mark never uses.
    """
    width, height = image.size
    cx, cy = width / 2.0, height / 2.0
    found: list[tuple[int, int, int]] = []
    for step in range(0, 360, 2):
        for radius in (0.36, 0.38, 0.40, 0.42):
            x = int(cx + radius * width * math.cos(math.radians(step)))
            y = int(cy + radius * height * math.sin(math.radians(step)))
            if not (0 <= x < width and 0 <= y < height):
                continue
            pixel = image.getpixel((x, y))
            r, g, b, a = int(pixel[0]), int(pixel[1]), int(pixel[2]), int(pixel[3])
            if a < 240:
                continue
            if (r + g + b) / 3.0 > 80:  # skip the lettering
                continue
            found.append((r, g, b))
    if not found:
        return "#1b1f26"
    found.sort(key=lambda rgb: sum(rgb))
    mid = found[len(found) // 2]
    return "#%02x%02x%02x" % mid


def write_android(app: Image.Image) -> None:
    write_png(app, ANDROID_RES / "mipmap" / "icon.png", 512)
    for density, size in ANDROID_DENSITIES.items():
        write_png(app, ANDROID_RES / f"mipmap-{density}" / "icon.png", size)
    write_png(app, ANDROID_RES / "drawable-xxhdpi" / "ic_launcher_foreground.png",
              ADAPTIVE_FOREGROUND_PX)
    write_png(app, QT_ANDROID_RES / "mipmap" / "icon.png", 256)

    background = plate_colour(app)
    colours = ANDROID_RES / "values" / "colors.xml"
    colours.write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<resources>\n"
        f'    <!-- Sampled from the plate inside docs/branding/app-icon.png so the\n'
        f"         launcher mask shows the same dark field the mark already has. -->\n"
        f'    <color name="ic_launcher_background">{background}</color>\n'
        "</resources>\n",
        encoding="utf-8",
    )
    print(f"{colours.relative_to(ROOT)}  ic_launcher_background {background}")

    adaptive = ANDROID_RES / "mipmap-anydpi-v26" / "icon.xml"
    adaptive.parent.mkdir(parents=True, exist_ok=True)
    adaptive.write_text(
        '<?xml version="1.0" encoding="utf-8"?>\n'
        "<adaptive-icon xmlns:android=\"http://schemas.android.com/apk/res/android\">\n"
        '    <background android:drawable="@color/ic_launcher_background" />\n'
        '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
        "</adaptive-icon>\n",
        encoding="utf-8",
    )
    print(f"{adaptive.relative_to(ROOT)}  adaptive icon (API 26+)")


def write_linux(app: Image.Image, package: Image.Image) -> None:
    for size in HICOLOR_SIZES:
        write_png(app, LINUX_RES / "icons" / "hicolor" / f"{size}x{size}" / "apps"
                  / "MegaVaultProtect.png", size)
    for size in HICOLOR_SIZES[:-1]:  # file icons stop at 256
        write_png(package, LINUX_RES / "icons" / "hicolor" / f"{size}x{size}" / "mimetypes"
                  / "application-x-megavault-package.png", size)


def main() -> None:
    app = load(APP_MASTER)
    package = load(PACKAGE_MASTER)

    # README and docs
    write_png(app, ROOT / "docs" / "MVP-logo.png", 1024)
    write_png(app, ROOT / "docs" / "MVP-logo-banner.png", 1024)
    write_png(package, ROOT / "docs" / "MVP-package-logo.png", 1024)

    # Qt app: window icon plus the two Windows resources
    write_png(app, QT_RES / "app.png", 256)
    write_ico(app, QT_RES / "app.ico")
    write_ico(package, QT_RES / "mvpk.ico")

    write_android(app)
    write_linux(app, package)


if __name__ == "__main__":
    main()
