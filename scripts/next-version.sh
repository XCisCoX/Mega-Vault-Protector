#!/usr/bin/env bash
# Print the next unused version tag.
# Stable tags look like v2.0.4. Beta tags look like v2.0.4-beta.1.
# Date tags and anything else are ignored, so a calendar tag cannot
# jump the version ahead. The printed tag is not created here.
set -euo pipefail

beta=0
if [ "${1:-}" = "--beta" ]; then
    beta=1
fi

# Floor of the upcoming release line. Tags below it (the 2.0.x line) cannot
# pull the next version back down: after the last 2.0 tag this yields 2.1.0.
floor_major=2
floor_minor=1

major=2
minor=0
patch=3
found=0

while IFS= read -r tag; do
    [ -n "$tag" ] || continue
    if [[ "$tag" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
        a="${BASH_REMATCH[1]}"
        b="${BASH_REMATCH[2]}"
        c="${BASH_REMATCH[3]}"
        if [ "$found" -eq 0 ] \
            || [ "$a" -gt "$major" ] \
            || { [ "$a" -eq "$major" ] && [ "$b" -gt "$minor" ]; } \
            || { [ "$a" -eq "$major" ] && [ "$b" -eq "$minor" ] && [ "$c" -gt "$patch" ]; }; then
            major="$a"
            minor="$b"
            patch="$c"
            found=1
        fi
    fi
done < <(git tag -l 'v*')

if [ "$found" -eq 1 ]; then
    if [ "$major" -lt "$floor_major" ] \
        || { [ "$major" -eq "$floor_major" ] && [ "$minor" -lt "$floor_minor" ]; }; then
        major="$floor_major"
        minor="$floor_minor"
        patch=0
    else
        patch=$((patch + 1))
    fi
fi

if [ "$beta" -eq 1 ]; then
    n=1
    candidate="v${major}.${minor}.${patch}-beta.${n}"
    while git rev-parse -q --verify "refs/tags/${candidate}" >/dev/null; do
        n=$((n + 1))
        candidate="v${major}.${minor}.${patch}-beta.${n}"
    done
else
    candidate="v${major}.${minor}.${patch}"
    while git rev-parse -q --verify "refs/tags/${candidate}" >/dev/null; do
        patch=$((patch + 1))
        candidate="v${major}.${minor}.${patch}"
    done
fi

printf '%s\n' "$candidate"
