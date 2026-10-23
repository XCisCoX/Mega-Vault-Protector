#!/usr/bin/env bash
# Ubuntu's SQLCipher 3 cannot read this application's SQLCipher 4 vaults.
# Build a pinned community SQLCipher 4 release without changing system libraries.
set -euo pipefail

revision=c5bd336ece77922433aaf6d6fe8cf203b0c299d5 # upstream v4.6.1
prefix="${1:?usage: build-linux-sqlcipher.sh INSTALL_PREFIX}"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT

git init -q "$work/source"
git -C "$work/source" remote add origin https://github.com/sqlcipher/sqlcipher.git
git -C "$work/source" fetch -q --depth 1 origin "$revision"
git -C "$work/source" checkout -q --detach FETCH_HEAD
test "$(git -C "$work/source" rev-parse HEAD)" = "$revision"
(
    cd "$work/source"
    ./configure --prefix="$prefix" --enable-tempstore=yes --disable-tcl \
        --disable-readline --enable-shared --disable-static \
        CFLAGS="-O2 -fPIC -DSQLITE_HAS_CODEC -DSQLITE_TEMP_STORE=2" LDFLAGS="-lcrypto"
    make -j2
    make install
)
mkdir -p "$prefix/share/licenses/sqlcipher"
cp "$work/source/LICENSE.md" "$prefix/share/licenses/sqlcipher/LICENSE"
version="$(LD_LIBRARY_PATH="$prefix/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$prefix/bin/sqlcipher" :memory: 'PRAGMA cipher_version;')"
[[ "$version" == 4.6.1* ]] || { printf 'Unexpected SQLCipher runtime: %s\n' "$version" >&2; exit 1; }
printf 'SQLCipher runtime verified: %s (%s)\n' "$version" "$revision"
if [[ -n "${GITHUB_ENV:-}" ]]; then
    {
        printf 'PKG_CONFIG_PATH=%s/lib/pkgconfig%s\n' "$prefix" "${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
        printf 'LD_LIBRARY_PATH=%s/lib%s\n' "$prefix" "${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        printf 'MVP_SQLCIPHER_LICENSE=%s/share/licenses/sqlcipher/LICENSE\n' "$prefix"
    } >> "$GITHUB_ENV"
fi
