#!/bin/sh
# Register this extracted build with the current desktop session: application
# menu entry, the hicolor icon set, and the .mvpk package type with its own
# icon. Everything lands in the user's XDG data directory, so it needs no root.
# Re-run it after moving the folder. Delete the files listed at the end to undo.
set -eu

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
LAUNCHER="$HERE/MegaVaultProtect.sh"

if [ ! -x "$LAUNCHER" ]; then
    echo "Extract the archive first: $LAUNCHER is missing." >&2
    exit 1
fi

install -d "$DATA/applications" "$DATA/mime/packages" "$DATA/icons"

# Desktop entry: the Exec line has to be absolute, because a menu entry has no
# working directory of its own.
sed "s|@EXEC@|$LAUNCHER|" "$HERE/MegaVaultProtect.desktop" \
    > "$DATA/applications/MegaVaultProtect.desktop"
chmod 644 "$DATA/applications/MegaVaultProtect.desktop"

# Icons and the package type share the hicolor tree, so one copy is enough.
cp -R "$HERE/icons/hicolor" "$DATA/icons/"
cp "$HERE/megavault-package.xml" "$DATA/mime/packages/megavault-package.xml"

if command -v update-mime-database >/dev/null 2>&1; then
    update-mime-database "$DATA/mime" || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$DATA/applications" || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$DATA/icons/hicolor" >/dev/null 2>&1 || true
fi
if command -v xdg-mime >/dev/null 2>&1; then
    # Makes the package icon the default handler for .mvpk in this session.
    xdg-mime default MegaVaultProtect.desktop application/x-megavault-package || true
fi

echo "Installed for ${USER:-$(id -un 2>/dev/null || echo this user)}:"
echo "  $DATA/applications/MegaVaultProtect.desktop"
echo "  $DATA/icons/hicolor/*/apps/MegaVaultProtect.png"
echo "  $DATA/icons/hicolor/*/mimetypes/application-x-megavault-package.png"
echo "  $DATA/mime/packages/megavault-package.xml"
