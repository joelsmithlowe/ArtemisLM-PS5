#!/usr/bin/env bash
# Bakes the dashboard's typeface, Manrope (OFL, third_party/fonts/manrope), into
# the SDF .huifont files the app loads from assets/fonts, with accented Latin,
# typographic quotes and dashes (the european set), as models write them: weight 500 for text,
# 600 for titles and 200 for the large numbers. Needs fontTools and a host clang++.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
kit="$root/third_party/ps5-homebrew-ui"
src="$root/third_party/fonts/manrope"
out="$root/ps5/app/assets/fonts"
tmp="$root/build/fonts"
mkdir -p "$tmp" "$out"
"${HOST_CXX:-clang++}" -std=c++20 -O2 -w "$kit/tools/font-baker/bake_font.cpp" -o "$tmp/bake_font"
for weight in 200 500 600; do
    python3 -c 'import sys
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
instantiateVariableFont(TTFont(sys.argv[1]), {"wght": int(sys.argv[2])}).save(sys.argv[3])' \
        "$src/Manrope[wght].ttf" "$weight" "$tmp/manrope-$weight.ttf"
    "$tmp/bake_font" "$tmp/manrope-$weight.ttf" "$out/manrope-$weight.huifont" 64 8 2048 european
done
cp "$src/OFL.txt" "$out/manrope-LICENSE.txt"
echo "bake-fonts: $out"
