"""Guards for the Android surfaces that must match the desktop app.

The desktop puts every dialog in a dimmed, blurred window over the app and keeps
one sheet style for plugin consent/settings. These checks fail if a new dialog
slips in with the old plain window or Material AlertDialog shape.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

APP = Path(__file__).resolve().parents[1] / 'app' / 'src' / 'main' / 'java' / 'org' / 'megavideoprotect' / 'app'
COMPONENTS = (APP / 'Components.kt').read_text(encoding='utf-8')


def sources() -> dict[str, str]:
    return {p.name: p.read_text(encoding='utf-8') for p in APP.glob('*.kt')}


class DialogSurfaces(unittest.TestCase):
    def test_blur_helper_exists_and_uses_platform_blur(self):
        self.assertIn('fun BlurredDialogWindow(', COMPONENTS)
        self.assertIn('setBackgroundBlurRadius', COMPONENTS)
        self.assertIn('FLAG_BLUR_BEHIND', COMPONENTS)
        # Platform blur exists from Android 12; older devices keep the dim only.
        self.assertIn('Build.VERSION.SDK_INT >= Build.VERSION_CODES.S', COMPONENTS)

    def test_one_sheet_style(self):
        for required in ('title', 'onDismiss', 'confirmLabel', 'dismissLabel', 'confirmEnabled'):
            self.assertIn(f'{required}', COMPONENTS)
        self.assertIn('fun MvpSheet(', COMPONENTS)
        # The body scrolls and the buttons stay pinned below it.
        self.assertIn('verticalScroll(rememberScrollState())', COMPONENTS)
        self.assertIn('heightIn(max = 420.dp)', COMPONENTS)

    def test_every_dialog_gets_the_blurred_backdrop(self):
        """A bare Dialog( is a dialog with a plain window: that is the bug class."""
        offenders = []
        for name, text in sources().items():
            if name in {'SettingsDialog.kt', 'TransferHub.kt'}:
                continue  # settings has its own blur; TransferPage is a full page
            for match in re.finditer(r'(?<![A-Za-z])Dialog\(', text):
                window = text[match.start():match.start() + 900]
                if 'BlurredDialogWindow' not in window and 'MvpSheet' not in window:
                    line = text[:match.start()].count('\n') + 1
                    offenders.append(f'{name}:{line}')
        self.assertEqual([], offenders, 'dialogs without the blurred backdrop')

    def test_no_material_alert_dialogs_left(self):
        for name, text in sources().items():
            self.assertNotIn('AlertDialog(', text, f'{name} still uses a Material AlertDialog')

    def test_plugin_surfaces_use_the_sheet(self):
        plugin = (APP / 'PluginScreen.kt').read_text(encoding='utf-8')
        self.assertGreaterEqual(plugin.count('MvpSheet('), 3)
        # Consent, extension settings and music settings each confirm explicitly.
        self.assertIn('Approve and enable', plugin)
        self.assertIn('confirmEnabled', plugin)


class ProviderFolderIcons(unittest.TestCase):
    def test_back_control_is_an_asset_not_a_glyph(self):
        library = (APP / 'PluginLibrary.kt').read_text(encoding='utf-8')
        self.assertIn('ic_lucide_arrow_left', library)
        self.assertNotIn('"‹', library)

    def test_download_and_saved_use_lucide(self):
        library = (APP / 'PluginLibrary.kt').read_text(encoding='utf-8')
        self.assertIn('ic_lucide_download', library)
        self.assertIn('ic_lucide_check', library)

    def test_icon_assets_exist_in_the_pack(self):
        pack = Path(__file__).resolve().parents[2] / 'assets' / 'icons' / 'android' / 'drawable'
        for name in ('ic_lucide_arrow_left', 'ic_lucide_download', 'ic_lucide_check', 'ic_lucide_folder_open'):
            self.assertTrue((pack / f'{name}.xml').is_file(), name)


if __name__ == '__main__':
    unittest.main()
