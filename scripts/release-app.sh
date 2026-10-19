#!/usr/bin/env bash
# Packages the built app for a release: build/release/artemislm-ps5-app-v<version>.zip
# holding the app's title folder (PPSA98010), plus its sha256. The version comes from
# ps5/app/version.hpp; the git tag is v<version>.
#
#   scripts/build-app.sh && scripts/release-app.sh
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
version=$(sed -n 's/^#define PS5LM_VERSION "\(.*\)"$/\1/p' "$root/ps5/app/version.hpp")
title=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$root/ps5/app/sce_sys/param.json")
app="$root/build/app-ps5/title/$title"
out="$root/build/release"
zip="$out/artemislm-ps5-app-v$version.zip"

[ -f "$app/eboot.bin" ] || { echo "release-app: build it first (scripts/build-app.sh)" >&2; exit 1; }
grep -q "^## \[$version\]" "$root/CHANGELOG.md" || { echo "release-app: CHANGELOG.md has no [$version] entry" >&2; exit 1; }
# The console compares contentVersion (NN.NNN.NNN) to tell an installed copy is
# outdated: major+1, minor, patch, so 0.8.2 ships as 01.008.002.
IFS=. read -r major minor patch <<< "$version"
content=$(printf '%02d.%03d.%03d' $((major + 1)) "$minor" "$patch")
grep -q "\"contentVersion\": \"$content\"" "$root/ps5/app/sce_sys/param.json" ||
    { echo "release-app: set contentVersion to $content in ps5/app/sce_sys/param.json" >&2; exit 1; }
mkdir -p "$out"
rm -f "$zip"
(cd "$(dirname "$app")" && zip -qr -X "$zip" "$title")
# One title only. Another id, or the old ps5lm.elf payload, is a second install.
python3 - "$zip" "$title" <<'PY' || exit 1
import sys, zipfile
zip_path, title = sys.argv[1], sys.argv[2]
bad = False
with zipfile.ZipFile(zip_path) as z:
    for name in z.namelist():
        base = name.rsplit("/", 1)[-1]
        if base == "ps5lm.elf":
            print("release-app: zip contains ps5lm.elf", file=sys.stderr)
            bad = True
        if name.split("/", 1)[0] != title:
            print("release-app: zip member is not under %s: %s" % (title, name), file=sys.stderr)
            bad = True
sys.exit(1 if bad else 0)
PY
(cd "$out" && sha256sum "$(basename "$zip")" > "$(basename "$zip").sha256")
echo "release-app: $zip ($(du -h "$zip" | cut -f1))"
cat "$zip.sha256"
