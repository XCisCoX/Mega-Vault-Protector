"""Every colour a widget stylesheet uses must be translatable to the light theme.

theme.cpp maps dark hex/rgba literals to their light counterparts. A literal that
is missing from that map survives untouched, so the light theme silently shows a
dark tree, folder or text colour. This guard reads the map out of theme.cpp and
checks every colour declaration in the Qt sources against it.

Player windows are exempt: they are dark media surfaces by design
(preserveDarkTheme / PlayerWindow / PdfWindow).
"""
import re
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SOURCES = REPO / "qt-app" / "src"
THEME = SOURCES / "theme.cpp"

# Files that are deliberately dark in both themes.
EXEMPT = {"theme.cpp", "player_window.cpp", "pdf_window.cpp", "music_window.cpp", "windows_fallback.cpp"}

# Semantic colours that stay identical in both themes on purpose (accent blue,
# its hover, and the light-theme error red). They are declared here instead of
# in theme.cpp so the transform never rewrites them.
INVARIANT = {"#3390ec", "#4ba0f5", "#5aa6f0", "#c62828"}

DECLARATION = re.compile(r"(?<![\w-])(color|background(?:-color)?|border(?:-[a-z]+)?)\s*:\s*([^;{}\"]+)")
COLOR_TOKEN = re.compile(r"#[0-9a-fA-F]{3,8}\b|rgba?\([^)]*\)|\bwhite\b", re.IGNORECASE)


def mapped_colours() -> set[str]:
    """Keys of the QHash<QString, QString> colours table in theme.cpp."""
    text = THEME.read_text(encoding="utf-8")
    table = text.split("static const QHash<QString, QString> colors{", 1)[1]
    table = table.split("};", 1)[0]
    keys = set()
    for key in re.findall(r'\{\s*"([^"]+)"\s*,', table):
        keys.add(key.lower())
    return keys


def stylesheet_literals(path: Path):
    """Yield (line, colour) for colours inside setStyleSheet/append blocks only."""
    text = path.read_text(encoding="utf-8", errors="replace")
    for match in re.finditer(r"setStyleSheet\((.*?)\);", text, re.S):
        block = match.group(1)
        base = text[: match.start(1)].count("\n") + 1
        for line_offset, line in enumerate(block.splitlines()):
            declaration = DECLARATION.search(line)
            if not declaration:
                continue
            for token in COLOR_TOKEN.findall(declaration.group(2)):
                yield base + line_offset, token


class LightThemeCoverage(unittest.TestCase):
    def test_map_is_not_empty(self):
        self.assertGreater(len(mapped_colours()), 20, "colour map moved or renamed")

    def test_every_stylesheet_colour_is_mapped(self):
        mapped = mapped_colours()
        missing: dict[str, list[str]] = {}
        for path in sorted(SOURCES.glob("*.cpp")):
            if path.name in EXEMPT:
                continue
            for line, token in stylesheet_literals(path):
                value = token.lower()
                if value in {"transparent", "none", "inherit", "palette"}:
                    continue
                if value in mapped or value.replace(" ", "") in {m.replace(" ", "") for m in mapped}:
                    continue
                if value in INVARIANT:
                    continue
                # rgba(0,0,0,x) is a shadow/dim, valid on both themes.
                if value.startswith("rgba(0, 0, 0") or value.startswith("rgba(0,0,0"):
                    continue
                missing.setdefault(value, []).append(f"{path.name}:{line}")
        if missing:
            report = "\n".join(
                f"  {colour} -> {', '.join(where[:4])}" for colour, where in sorted(missing.items())
            )
            self.fail("stylesheet colours with no light-theme mapping:\n" + report)


if __name__ == "__main__":
    unittest.main()
