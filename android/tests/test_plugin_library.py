"""Integration source guards; Kotlin/JVM and device checks are separate."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'app/src/main/java/org/megavideoprotect/app'


class ProviderLibraryIntegration(unittest.TestCase):
    def test_one_mixed_list_songs_and_album_folders(self):
        source = (ROOT / 'PluginLibrary.kt').read_text(encoding='utf-8')
        # No Songs/Albums/Artists category level: songs are rows, albums folders.
        self.assertNotIn('PluginLibrary.category(', source)
        self.assertNotIn('"Artists"', source)
        self.assertIn('if (item.kind == "album")', source)
        self.assertIn('fun openAlbum(item: PluginItem)', source)
        self.assertIn('openAlbum(item)', source)
        self.assertIn('ProviderTrackRow(', source)

    def test_each_song_row_has_a_download_control_with_live_state(self):
        source = (ROOT / 'PluginLibrary.kt').read_text(encoding='utf-8')
        self.assertIn('DownloadControl(session.downloadStates[item.id]', source)
        self.assertIn("Downloading…", source)
        self.assertIn('"Saved"', source)
        session = (ROOT / 'PluginSession.kt').read_text(encoding='utf-8')
        self.assertIn('fun downloadTrack(item: PluginItem)', session)
        self.assertIn('nativeImportBytesForVault', session)
        self.assertIn('downloadPermits.withPermit', session)

    def test_discovery_uses_each_existing_gallery_view(self):
        source = (ROOT / 'VaultScreen.kt').read_text(encoding='utf-8')
        for view in ('DetailsView', 'IconGridView', 'ListView'):
            body = source.split('private fun ' + view + '(')[1]
            self.assertIn('providerSearch: List<PluginManifest>', body.split(') {')[0])
            self.assertIn('PluginSearchEntry(', body)
        self.assertIn('pluginManager.options(it).enabled', source)
        self.assertIn('PluginLibraryView(', source)

    def test_provider_ids_never_become_vault_file_ids(self):
        source = (ROOT / 'PluginLibrary.kt').read_text(encoding='utf-8')
        self.assertNotIn('nativeThumbnail', source)
        self.assertNotIn('VideoEntry(', source)
        self.assertNotIn('.toLong()', source)
        self.assertIn('PluginArtwork(', source)
        self.assertIn('FolderDetailRow(', source)
        self.assertIn('FolderListRow(', source)
        self.assertIn('FolderTile(', source)
        self.assertIn('onPlay(session)', source)

    def test_returns_to_folder_and_uses_existing_music_screen(self):
        source = (ROOT / 'MainActivity.kt').read_text(encoding='utf-8')
        self.assertIn('is Screen.PluginMusic ->', source)
        self.assertIn('pluginSession = session', source)
        self.assertIn('PluginLibrary.close()', source)
        library = (ROOT / 'PluginLibrary.kt').read_text(encoding='utf-8')
        self.assertNotIn('GalleryCache.currentFolderId =', library)
        self.assertIn('Back to library', library)
        self.assertIn('15_000', library)


if __name__ == '__main__':
    unittest.main()
