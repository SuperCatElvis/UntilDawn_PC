#!/usr/bin/env python3
"""make_icon.py <game_dir> [out.ico] - build untildawn.ico from the player's own sce_sys/icon0.png (512x512),
unchanged artwork, scaled to the sizes Windows uses (16..256). The icon is not part of the project's sources."""
import sys, os
from PIL import Image
src = os.path.join(sys.argv[1], 'sce_sys', 'icon0.png'); out = sys.argv[2] if len(sys.argv) > 2 else 'untildawn.ico'
Image.open(src).convert('RGBA').save(out, format='ICO', sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)])
print('wrote', out, os.path.getsize(out), 'bytes')
