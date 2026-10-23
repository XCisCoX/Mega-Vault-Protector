#!/usr/bin/env bash
# Bundle MegaVaultProtect with the Qt, FFmpeg, and GStreamer libraries it
# loads, so the release tarball runs without a matching -dev install.
# Glibc itself stays on the system (Ubuntu 24.04 / glibc 2.39 or newer).
set -euo pipefail

bin="${1:?path to the MegaVaultProtect binary}"
archive="${2:?output tar.gz path}"

if [[ ! -x "$bin" ]]; then
  echo "Release binary not found: $bin" >&2
  exit 1
fi

stage="$(mktemp -d)"
root="$stage/MegaVaultProtect"
mkdir -p "$root/lib" "$root/plugins" "$root/gstreamer-1.0"

cp -L "$bin" "$root/MegaVaultProtect"
chmod +x "$root/MegaVaultProtect"
# The desktop build stages original license texts beside its executable.
mkdir -p "$root/licenses"
cp -a "$(dirname "$bin")/licenses/telegram-player" "$root/licenses/"

is_system_lib() {
  case "$1" in
    */libc.so.*|*/libm.so.*|*/libdl.so.*|*/libpthread.so.*|*/librt.so.*|*/libresolv.so.*|*/libutil.so.*|*/ld-linux-x86-64.so.*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

declare -A seen=()
queue=()

enqueue() {
  local file="$1"
  [[ -f "$file" ]] || return 0
  local real
  real="$(readlink -f "$file")"
  if [[ -n "${seen[$real]:-}" ]]; then
    return 0
  fi
  seen["$real"]=1
  queue+=("$real")
}

collect_deps() {
  local file="$1"
  local dep
  local listing
  listing="$(ldd "$file" 2>/dev/null || true)"
  while read -r dep; do
    [[ -n "$dep" && -f "$dep" ]] || continue
    if is_system_lib "$dep"; then
      continue
    fi
    enqueue "$dep"
  done < <(printf '%s\n' "$listing" | awk '/=>/ { print $3 }')
}

export QT_SELECT=qt5
plugindir=""
if command -v qmake >/dev/null 2>&1; then
  plugindir="$(qmake -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
fi
if [[ -z "$plugindir" || ! -d "$plugindir" ]]; then
  plugindir=/usr/lib/x86_64-linux-gnu/qt5/plugins
fi

for sub in platforms platforminputcontexts imageformats mediaservice audio iconengines xcbglintegrations; do
  if [[ -d "$plugindir/$sub" ]]; then
    mkdir -p "$root/plugins/$sub"
    cp -a "$plugindir/$sub/." "$root/plugins/$sub/"
  fi
done

gst_dir=/usr/lib/x86_64-linux-gnu/gstreamer-1.0
if [[ -d "$gst_dir" ]]; then
  cp -a "$gst_dir/." "$root/gstreamer-1.0/"
fi

enqueue "$root/MegaVaultProtect"
while IFS= read -r -d '' so; do
  enqueue "$so"
done < <(find "$root/plugins" "$root/gstreamer-1.0" -type f -name '*.so' -print0)

# Qt's xcb plugin loads these with dlopen, so ldd never sees them. A desktop
# session then reports that the platform plugin failed to initialize.
for extra in libxcb-cursor.so.0 libxcb-xinerama.so.0; do
  found="$(ldconfig -p 2>/dev/null | awk -v name="$extra" '$1 == name { print $NF; exit }')"
  if [[ -n "$found" && -e "$found" ]]; then
    enqueue "$found"
  fi
done

index=0
while [[ "$index" -lt "${#queue[@]}" ]]; do
  collect_deps "${queue[$index]}"
  index=$((index + 1))
done

# ldd names are symlinks (libQt5Core.so.5 -> libQt5Core.so.5.15.x). The queue
# stores the real file, and the loader asks for the symlink name. Copy the
# real file, then add the SONAME the binary was linked against.
link_soname() {
  local file="$1"
  local soname base
  soname="$(readelf -d "$file" 2>/dev/null | awk -F'[][]' '/SONAME/ { print $2; exit }')"
  base="$(basename "$file")"
  if [[ -n "$soname" && "$soname" != "$base" ]]; then
    ln -sfn "$base" "$root/lib/$soname"
  fi
}

app_real="$(readlink -f "$root/MegaVaultProtect")"
for file in "${queue[@]}"; do
  [[ "$file" == "$app_real" ]] && continue
  case "$file" in
    "$root"/*) continue ;;
  esac
  cp -L "$file" "$root/lib/$(basename "$file")"
  link_soname "$root/lib/$(basename "$file")"
done

is_glibc_name() {
  case "$1" in
    libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|librt.so.*|libresolv.so.*|libutil.so.*|ld-linux-x86-64.so.*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

missing=0
check_needed() {
  local file="$1"
  local needed
  while read -r needed; do
    [[ -n "$needed" ]] || continue
    if is_glibc_name "$needed"; then
      continue
    fi
    if [[ -e "$root/lib/$needed" || -e "$(dirname "$file")/$needed" ]]; then
      continue
    fi
    echo "Missing library $needed required by ${file#"$root"/}" >&2
    missing=1
  done < <(readelf -d "$file" 2>/dev/null | awk -F'[][]' '/\(NEEDED\)/ { print $2 }')
}

check_needed "$root/MegaVaultProtect"
while IFS= read -r -d '' so; do
  check_needed "$so"
done < <(find "$root/lib" "$root/plugins" "$root/gstreamer-1.0" -type f -name '*.so*' -print0)
if [[ "$missing" -ne 0 ]]; then
  echo "The Linux bundle is missing libraries, so it will not start." >&2
  exit 1
fi

cat > "$root/qt.conf" << 'EOF'
[Paths]
Prefix=.
Plugins=plugins
Libraries=lib
EOF

cat > "$root/MegaVaultProtect.sh" << 'EOF'
#!/bin/sh
HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$HERE/plugins"
export QT_QPA_PLATFORM_PLUGIN_PATH="$HERE/plugins/platforms"
export GST_PLUGIN_PATH="$HERE/gstreamer-1.0"
export GST_PLUGIN_SYSTEM_PATH_1_0="$HERE/gstreamer-1.0"
# This archive ships the xcb plugin. Leave the variable alone when the user
# already chose a platform.
if [ -z "${QT_QPA_PLATFORM:-}" ]; then
  export QT_QPA_PLATFORM=xcb
fi
exec "$HERE/MegaVaultProtect" "$@"
EOF
chmod +x "$root/MegaVaultProtect.sh"

# Desktop integration. The archive stays portable: install-desktop-entry.sh
# copies the menu entry, the hicolor icons and the .mvpk package type into the
# user's XDG directories only when they ask for it.
here="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
linux_res="$here/qt-app/resources/linux"
if [[ ! -d "$linux_res/icons" ]]; then
  echo "Linux icon resources not found under $linux_res" >&2
  exit 1
fi
cp -R "$linux_res/icons" "$root/icons"
cp "$linux_res/MegaVaultProtect.desktop.in" "$root/MegaVaultProtect.desktop"
cp "$linux_res/megavault-package.xml" "$root/megavault-package.xml"
cp "$linux_res/install-desktop-entry.sh" "$root/install-desktop-entry.sh"
chmod +x "$root/install-desktop-entry.sh"

cat > "$root/README.txt" << 'EOF'
Mega Vault Protect for Linux x64.

Run:
  ./MegaVaultProtect.sh

This build is linked on Ubuntu 24.04. It needs glibc 2.39 or newer
(Ubuntu 24.04, or another current distro). Extract the archive and
start the shell script above; the libraries it needs are in lib/.

Optional: add the application menu entry, the icons, and the .mvpk
package type (so package files show their own icon and open here):
  ./install-desktop-entry.sh
EOF

tar -C "$stage" -czf "$archive" MegaVaultProtect
echo "Packaged $archive ($(wc -c < "$archive") bytes)"
