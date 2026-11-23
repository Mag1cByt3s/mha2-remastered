#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ICON_DST="$HOME/.local/share/icons/mha2-remastered.png"
DESKTOP_DST="$HOME/.local/share/applications/mha2-remastered.desktop"

mkdir -p "$(dirname "$ICON_DST")" "$(dirname "$DESKTOP_DST")"

# Generate a crisp 256x256 square icon from the 4K Moorhuhn sprite if present
SRC_SPR="$ROOT_DIR/hd_assets/sprites/s/001/img/walkS_f000.webp"
if [ ! -f "$SRC_SPR" ]; then
    SRC_SPR="$ROOT_DIR/hd_assets/sprites/s/001/img/walkS_f000.png"
fi
if [ -f "$SRC_SPR" ]; then
    SRC_SPR="$SRC_SPR" ICON_DST="$ICON_DST" nix-shell -p python3Packages.pillow --run 'python3 -c "
import os
from PIL import Image
im = Image.open(os.environ[\"SRC_SPR\"]).convert(\"RGBA\")
bbox = im.getbbox()
if bbox:
    im = im.crop(bbox)
# Focus on Moorhuhn upper body/head for a super-clean launcher icon
w, h = im.size
side = max(w, int(h * 0.72))
crop = im.crop((0, 0, w, min(h, side)))
canvas = Image.new(\"RGBA\", (side, side), (20, 24, 38, 255))
canvas.paste(crop, ((side - crop.width) // 2, (side - crop.height) // 2), crop)
canvas.resize((256, 256), Image.LANCZOS).save(os.environ[\"ICON_DST\"])
"'
fi

cat > "$DESKTOP_DST" <<EOF
[Desktop Entry]
Type=Application
Name=Moorhuhn Adventure 2: Remastered v5
Comment=Native Linux 4K Neural Remaster of Moorhuhn Adventure 2: Der Fluch des Goldes
Exec="$ROOT_DIR/play.sh"
Path=$ROOT_DIR
Icon=$ICON_DST
Terminal=false
Categories=Game;AdventureGame;
StartupWMClass=mha2_linux
EOF

chmod +x "$DESKTOP_DST"
echo "Installed desktop entry: $DESKTOP_DST"
echo "Installed launcher icon: $ICON_DST"
