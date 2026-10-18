#!/usr/bin/env bash
# Renders the app's artwork from its SVG sources in ps5/app/sce_sys/source:
# icon0.png (512x512) and background.png (3840x2160), which scripts/build-app.sh
# turns into the home screen's DDS backgrounds. The text is set in the site's
# Recursive fonts (site/assets/fonts), installed for fontconfig as "PS5LM Mono"
# and "PS5LM Sans" by scripts/artwork-fonts.py. Needs rsvg-convert and fontTools.
set -euo pipefail

# icon0.png and source/background.png are the starflight mark. Running the
# SVG render replaces that mark with the old wordmark.
if [ "${ARTEMISLM_RENDER_WORDMARK:-}" != 1 ]; then
    echo "render-artwork: keeping the starflight mark (set ARTEMISLM_RENDER_WORDMARK=1 to draw the SVG wordmark)"
    exit 0
fi

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
src="$root/ps5/app/sce_sys/source"
fonts="${XDG_DATA_HOME:-$HOME/.local/share}/fonts"
python3 "$root/scripts/artwork-fonts.py" "$root/site/assets/fonts" "$fonts" > /dev/null
fc-cache -f "$fonts"
rsvg-convert -w 512 -h 512 "$src/icon.svg" -o "$root/ps5/app/sce_sys/icon0.png"
rsvg-convert -w 3840 -h 2160 "$src/background.svg" -o "$src/background.png"
echo "render-artwork: icon0.png and source/background.png"
