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
    def test_lucide_vectors_are_the_icon_family(self):
        """Every ic_lucide_* reference resolves to a generated vector, and no
        retired bitmap icon is referenced any more."""
        vectors = {
            p.stem for p in (Path(__file__).resolve().parents[2] / "assets/icons/android/drawable").glob("*.xml")
        }
        self.assertGreater(len(vectors), 30, "Lucide vector set is missing")
        referenced = set()
        retired = {
            "ic_ab_back", "msg_close", "msg_settings", "msg_add", "msg_download",
            "outline_header_search", "outline_header_lock_24", "menu_tag_filter",
            "files_folder", "msg_delete_filled", "checkbig", "msg_check_s",
        }
        for path in APP.glob("*.kt"):
            text = path.read_text(encoding="utf-8")
            referenced |= set(re.findall(r"R\.drawable\.(ic_lucide_[a-z0-9_]+)", text))
            leftover = retired & set(re.findall(r"R\.drawable\.([a-z0-9_]+)", text))
            self.assertFalse(leftover, f"{path.name} still uses retired bitmap icons: {sorted(leftover)}")
        self.assertTrue(referenced, "no Lucide icon is used yet")
        missing = sorted(referenced - vectors)
        self.assertFalse(missing, f"vectors not generated: {missing}")

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
