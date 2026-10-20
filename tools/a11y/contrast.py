#!/usr/bin/env python3
"""WCAG contrast for the app's theme tokens, read from the source of truth.

The palettes are plain hex literals in theme.hpp (dark) and theme.cpp (light), so this
reads them instead of keeping a copy that drifts. Text/background pairs are declared
below with the surface they actually sit on, because only the caller knows that.

Usage:  python tools/a11y/contrast.py [--min 4.5]
Exit:   0 when every pair passes, 1 otherwise (so CI can gate on it).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
THEME_HPP = ROOT / "qt-app/include/videovault/app/theme.hpp"
THEME_CPP = ROOT / "qt-app/src/theme.cpp"

# What has to be legible, and on what. Each entry is (label, text role, background role,
# whether the text is large/bold enough for the 3.0 threshold).
PAIRS = [
    ("window text", "WindowText", "Window", False),
    ("field text", "Text", "Base", False),
    ("button label", "ButtonText", "Button", False),
    ("primary button label", "HighlightedText", "Highlight", False),
    ("placeholder", "PlaceholderText", "Base", False),
    ("disabled field text", "Disabled:Text", "Base", False),
    ("disabled button label", "Disabled:ButtonText", "Button", False),
    ("selection label", "HighlightedText", "Highlight", False),
]

# setColor(QPalette::<Role>[, QPalette::<Group>], <colour>) where the colour is one of
# QColor(QStringLiteral("#rrggbb")), QColor(r, g, b) or Qt::white/Qt::black. The two
# palettes in this codebase use different spellings, so all three have to be read.
SET_COLOR_RE = re.compile(
    r"setColor\(\s*QPalette::(\w+)\s*(?:,\s*QPalette::(\w+)\s*)?,\s*"
    r"(?:QColor\(\s*(?:QStringLiteral\(\s*\"(#[0-9a-fA-F]{3,8})\"\s*\)"
    r"|(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3}))"
    r"|(Qt::white|Qt::black))")

NAMED = {"Qt::white": "#ffffff", "Qt::black": "#000000"}


def parse_palette(text: str, start_marker: str) -> dict[str, str]:
    """Collects QPalette::Role -> #rrggbb for one palette function."""
    start = text.index(start_marker)
    end = text.index("\n}", start)
    body = text[start:end]
    roles: dict[str, str] = {}
    for match in SET_COLOR_RE.finditer(body):
        role, group, hex_color, r, g, b, named = match.groups()
        if hex_color:
            value = hex_color.lower()
        elif named:
            value = NAMED[named]
        else:
            value = "#{:02x}{:02x}{:02x}".format(int(r), int(g), int(b))
        # setColor(group, role, colour) puts the group first; setColor(role, colour) has none.
        key = f"{role}:{group}" if group is not None else role
        roles[key] = value
    return roles


def relative_luminance(hex_color: str) -> float:
    r, g, b = (int(hex_color[i:i + 2], 16) / 255 for i in (1, 3, 5))

    def linear(channel: float) -> float:
        return channel / 12.92 if channel <= 0.03928 else ((channel + 0.055) / 1.055) ** 2.4

    return 0.2126 * linear(r) + 0.7152 * linear(g) + 0.0722 * linear(b)


def contrast_ratio(fg: str, bg: str) -> float:
    a, b = sorted((relative_luminance(fg), relative_luminance(bg)), reverse=True)
    return (a + 0.05) / (b + 0.05)


def colour_for(palette: dict[str, str], role: str) -> str | None:
    if role in palette:
        return palette[role]
    if ":" not in role:
        return palette.get(f"Active:{role}") or palette.get(f"Disabled:{role}")
    return None


def report(name: str, palette: dict[str, str], minimum: float) -> list[str]:
    failures = []
    print(f"\n{name}")
    print(f"  {'pair':26} {'fg':9} {'bg':9} {'ratio':>6}  verdict")
    for label, fg_role, bg_role, large in PAIRS:
        # Explicit roles win; a plain role may only be defined for one group.
        fg = colour_for(palette, fg_role)
        bg = colour_for(palette, bg_role)
        if fg is None or bg is None:
            # The dark palette leaves disabled colours to Qt and to the stylesheet, and
            # disabled text is exempt anyway, so a missing token here is informational.
            note = "exempt (no palette token)" if "disabled" in label else "missing token"
            print(f"  {label:26} {'?':9} {'?':9} {'-':>6}  {note}")
            if "disabled" not in label:
                failures.append(f"{name}: {label} has no token")
            continue
        ratio = contrast_ratio(fg, bg)
        needed = 3.0 if large else minimum
        exempt = "disabled" in label   # WCAG 1.4.3 exempts inactive components
        ok = ratio >= needed or exempt
        note = "exempt" if exempt and ratio < needed else ("pass" if ok else "FAIL")
        print(f"  {label:26} {fg:9} {bg:9} {ratio:6.2f}  {note} (needs {needed})")
        if not ok:
            failures.append(f"{name}: {label} {ratio:.2f} < {needed}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--min", type=float, default=4.5, help="minimum ratio for normal text")
    args = parser.parse_args()

    hpp = THEME_HPP.read_text(encoding="utf-8")
    cpp = THEME_CPP.read_text(encoding="utf-8")
    dark = parse_palette(hpp, "inline QPalette darkPalette()")
    light = parse_palette(cpp, "QPalette lightPalette()")

    print("WCAG 2.1 contrast, read from theme.hpp (dark) and theme.cpp (light)")
    print(f"threshold: {args.min} for normal text, 3.0 for large/icon text")
    failures = report("dark", dark, args.min) + report("light", light, args.min)

    print()
    if failures:
        print(f"{len(failures)} pair(s) below threshold:")
        for failure in failures:
            print(f"  - {failure}")
        return 1
    print("every declared pair meets its threshold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
