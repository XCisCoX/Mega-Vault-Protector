"""The tap that opens a video must not wait on the vault.

A tap used to resolve the kind of every file in the folder before the player
appeared — one authenticated vault read each, queued behind the gallery's
probes — so the first video on a fresh vault took a minute. These guards keep
that work off the tap path.
"""

import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
APP = ROOT / "android" / "app" / "src" / "main" / "java" / "org" / "megavideoprotect" / "app"


def read(name: str) -> str:
    return (APP / name).read_text(encoding="utf-8")


class TapPathTests(unittest.TestCase):
    def on_play_block(self) -> str:
        text = read("MainActivity.kt")
        start = text.index("onPlay = {")
        depth = 0
        for index in range(start, len(text)):
            if text[index] == "{":
                depth += 1
            elif text[index] == "}":
                depth -= 1
                if depth == 0:
                    return text[start:index]
        raise AssertionError("onPlay block not found")

    def test_the_tap_does_not_resolve_the_whole_playlist(self):
        block = self.on_play_block()
        self.assertNotRegex(
            block,
            r"playlist\.filter\s*\{[^}]*FileKinds\.resolve",
            "the tap reads a header per playlist entry again",
        )

    def test_the_tap_shows_a_screen_without_waiting_for_io(self):
        block = self.on_play_block()
        # The common paths must call show() directly, not inside withContext.
        self.assertIn("show(Screen.Player(", block)
        self.assertIn("show(Screen.Music(", block)
        self.assertIn("show(Screen.Image(", block)
        self.assertIn("show(Screen.Pdf(", block)

    def test_a_name_only_reading_exists(self):
        kinds = read("FileKinds.kt")
        self.assertIn("fun fromNameOnly(", kinds)
        self.assertIn("fun looksLikeVideo(", kinds)
        self.assertIn("fun looksLikeAudio(", kinds)


if __name__ == "__main__":
    unittest.main()
