"""Source wiring guards; Android/ExoPlayer execution is verified separately."""
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1] / 'app/src/main/java/org/megavideoprotect/app'

class PluginMusicWiring(unittest.TestCase):
    def test_existing_music_screen_can_use_session_without_vault_setup(self):
        source = (ROOT / 'MusicScreen.kt').read_text()
        self.assertIn('pluginSession: PluginSession? = null', source)
        self.assertIn('pluginSession.player', source)
        self.assertIn('pluginSession.selected', source)
        self.assertIn('pluginSession.playbackMessage', source)
        self.assertIn('Download to vault', source)
        self.assertIn('pluginSession.previous()', source)
        self.assertIn('pluginSession.next()', source)
        self.assertIn('if (pluginSession != null) return@LaunchedEffect', source)
        self.assertIn('if (pluginSession == null) {\n                        SongThumb', source)
        self.assertNotIn('pluginSession.close()', source)

    def test_catalogue_has_independent_deadline_and_playback_queue(self):
        source = (ROOT / 'PluginSession.kt').read_text()
        self.assertIn('var catalogueBusy by mutableStateOf(false)', source)
        self.assertIn('var playbackBusy by mutableStateOf(false)', source)
        self.assertIn('delay(15_000)', source)
        self.assertIn('catalogueGeneration', source)
        self.assertIn('catalogueRequests', source)
        self.assertIn('fun play(item: PluginItem, playlist: List<PluginItem> = rows)', source)
        self.assertIn('fun previous()', source)
        self.assertIn('fun next()', source)
        self.assertIn('Player.STATE_ENDED', source)
        self.assertIn('val canDownload:', source)

if __name__ == '__main__':
    unittest.main()
