"""Source guards for the sync list and the background runner.

The Android app cannot be compiled on this machine (no JDK or Android SDK), so
these checks pin the shape of the code that a compiler would otherwise be the
only witness to: one list with a selection, both apps acting on a selection in
one pass, and a sync that is owned by the process rather than by a screen.
"""

import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
APP = ROOT / "android" / "app" / "src" / "main" / "java" / "org" / "megavideoprotect" / "app"


def read(name: str) -> str:
    return (APP / name).read_text(encoding="utf-8")


class SyncListTests(unittest.TestCase):
    def test_the_screen_lists_items_with_a_checkbox_each(self):
        screen = read("SyncScreen.kt")
        self.assertIn("Checkbox(", screen)
        self.assertIn("Select all", screen)
        self.assertIn("items.take(", screen)

    def test_a_selection_acts_in_one_pass(self):
        screen = read("SyncScreen.kt")
        for call in ("downloadItems", "removeFromDevice", "tombstoneItems", "purgeItems"):
            self.assertIn(call, screen, f"the screen never calls {call}")

    def test_a_permanent_delete_asks_first(self):
        screen = read("SyncScreen.kt")
        self.assertIn("askingPurge", screen)
        self.assertIn("cannot be restored", screen)

    def test_the_engine_remembers_what_was_removed_here(self):
        engine = read("SyncV4Engine.kt")
        self.assertIn("remoteOnly.contains(record.id)", engine)
        self.assertIn("fun removeFromDevice(", engine)
        self.assertIn("fun items(", engine)

    def test_the_list_state_survives_a_restart(self):
        state = read("SyncV4State.kt")
        self.assertIn('"remoteOnly"', state)
        self.assertIn("val remoteOnly", state)


class BackgroundSyncTests(unittest.TestCase):
    def test_sync_runs_outside_the_screen(self):
        runner = read("SyncV4Run.kt")
        self.assertIn("object SyncV4Run", runner)
        self.assertIn("SupervisorJob()", runner)
        # A screen scope is what used to cancel the pass; it must not appear.
        self.assertNotIn("rememberCoroutineScope", runner)
        screen = read("SyncScreen.kt")
        self.assertIn("SyncV4Run.start(", screen)

    def test_the_footer_bar_shows_the_sync(self):
        hub = read("TransferHub.kt")
        self.assertIn("SyncV4Run.progress", hub)
        self.assertIn("Syncing", hub)

    def test_leaving_the_page_still_shows_progress(self):
        # The condition must come from the runner, not from screen state.
        hub = read("TransferHub.kt")
        self.assertIn("val sync = SyncV4Run.progress", hub)


if __name__ == "__main__":
    unittest.main()
