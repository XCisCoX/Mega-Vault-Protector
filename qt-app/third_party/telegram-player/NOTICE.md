# Telegram Desktop player assets and adaptations

Copyright (c) 2014–2026 The Telegram Desktop Authors.

Source project: <https://github.com/telegramdesktop/tdesktop>
Reference: the user-supplied Telegram Desktop 7.3.1 source snapshot
(`Telegram/build/version`: `AppVersion 7003001`). This snapshot has no Git
metadata, so no upstream commit identifier is asserted.

The original PNG coverage masks under `qt-app/resources/telegram/player/` are
copied without modification from `Telegram/Resources/icons/player/` in that
snapshot. `assets.json` records their relative paths and SHA-256 hashes. The
application interprets RGB intensity as coverage and applies the active UI
colour at runtime; it does not replace or modify the source PNGs.

Player control adaptations reference the supplied
`Telegram/SourceFiles/media/player/` and `Telegram/SourceFiles/media/view/`
sources and their `.style` definitions. They are adaptations for Mega Vault's
Qt widgets and encrypted media transport, not the upstream Telegram executable
or an assertion that all upstream dependencies have been linked.

Telegram Desktop supplies these materials under GNU GPL version 3 or, at your
option, any later version, with the OpenSSL exception described in the original
`LEGAL` file retained here. See the complete accompanying `LICENSE` and `LEGAL`.
Keep this attribution, the original license texts, and corresponding source
available when distributing adaptations. The surrounding Mega Vault project
uses GNU AGPL version 3; the GPLv3/AGPLv3 combination provisions apply without
relicensing the original Telegram material. This notice does not grant rights
to Telegram's name, logo, service, or other trademarks.

Mega Vault changes: runtime recolouring and high-DPI resource integration;
source-backed desktop control adaptations using the existing encrypted vault
reader. No Telegram branding, account/network code, or service credentials are
included.
