"""Screens must paint from the palette, so both themes work everywhere.

Raw Color(0xAARRGGBB) literals bypass MvpPalette and stay dark in light mode —
that is how folder tiles, the peer pane and transfer rows used to look. The two
media players are the deliberate exception: they are dark surfaces by design.

Run: python -m unittest discover -s android/tests
"""
import re
import unittest
from pathlib import Path

APP = Path(__file__).resolve().parents[1] / "app/src/main/java/org/megavideoprotect/app"
RAW = re.compile(r"Color\(0x[0-9A-Fa-f]{8}\)")
PLAYERS = {"Theme.kt", "MusicScreen.kt", "PlayerScreen.kt", "RemoteHttpDataSource.kt"}


class PaletteOnly(unittest.TestCase):
    def test_no_raw_colours_outside_the_palette_and_players(self):
        offenders = {}
        for path in sorted(APP.glob("*.kt")):
            if path.name in PLAYERS:
                continue
            hits = RAW.findall(path.read_text(encoding="utf-8"))
            if hits:
                offenders[path.name] = sorted(set(hits))
        if offenders:
            detail = "\n".join(f"  {name}: {', '.join(values)}" for name, values in offenders.items())
            self.fail("use the Mvp palette tokens, not raw colours:\n" + detail)

    def test_palette_carries_surface_tokens_for_both_themes(self):
        theme = (APP / "Theme.kt").read_text(encoding="utf-8")
        for token in ("tileBg", "tileText", "scrim", "progressTrack"):
            self.assertEqual(2, theme.count(f"{token} ="), f"{token} needs a dark and a light value")

    def test_tile_and_row_backgrounds_use_the_tokens(self):
        vault = (APP / "VaultScreen.kt").read_text(encoding="utf-8")
        self.assertIn("Mvp.tileBg", vault)
        self.assertIn("Mvp.tileText", vault)
        peer = (APP / "PeerScreen.kt").read_text(encoding="utf-8")
        self.assertIn("Mvp.tileBg", peer)


if __name__ == "__main__":
    unittest.main()
