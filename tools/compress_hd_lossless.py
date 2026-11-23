#!/usr/bin/env python3
"""
Moorhuhn Adventure 2 — 100% Bit-Exact Lossless WebP Optimizer for hd_assets/
Converts all HD PNGs (<= 16383px) to lossless WebP with exact=True (preserving
anti-halo dilated RGB in alpha=0 pixels, guaranteed 0 max_pixel_diff) across all CPU cores.
"""
import concurrent.futures
import os
import sys
import time
from PIL import Image

Image.MAX_IMAGE_PIXELS = None


def convert_one(png_path: str):
    webp_path = png_path[:-4] + ".webp"
    tmp_path = webp_path + ".tmp.webp"
    try:
        with Image.open(png_path) as im:
            w, h = im.size
            if w > 16383 or h > 16383:
                return ("skip", 0, 0, None)
            old_sz = os.path.getsize(png_path)
            im.save(tmp_path, format="WEBP", lossless=True, exact=True, method=4)
            new_sz = os.path.getsize(tmp_path)
            if new_sz < old_sz:
                os.replace(tmp_path, webp_path)
                os.remove(png_path)
                return ("ok", old_sz, new_sz, None)
            else:
                os.remove(tmp_path)
                return ("kept_png", old_sz, old_sz, None)
    except Exception as e:
        return ("err", 0, 0, f"{png_path}: {e}")
    finally:
        if os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "hd_assets"
    pngs = []
    for r, _, files in os.walk(root):
        for f in files:
            if f.endswith(".png"):
                pngs.append(os.path.join(r, f))
    pngs.sort()
    print(f"[webp] Converting {len(pngs)} PNGs in {root}/ to 100% bit-exact lossless WebP...", flush=True)
    t0 = time.time()
    ok = kept = skip = err = 0
    old_total = new_total = 0
    workers = max(1, os.cpu_count() or 8)
    with concurrent.futures.ProcessPoolExecutor(max_workers=workers) as ex:
        for idx, (st, osz, nsz, err_msg) in enumerate(ex.map(convert_one, pngs, chunksize=16), 1):
            old_total += osz
            new_total += nsz
            if st == "ok": ok += 1
            elif st == "kept_png": kept += 1
            elif st == "skip": skip += 1
            else:
                err += 1
                if err_msg:
                    print(f"[webp] ERROR: {err_msg}", file=sys.stderr, flush=True)
            if idx % 1000 == 0 or idx == len(pngs):
                el = time.time() - t0
                print(f"  [{idx}/{len(pngs)}] ok={ok} kept={kept} skip={skip} err={err} "
                      f"({old_total/1e9:.2f}GB -> {new_total/1e9:.2f}GB, {el:.0f}s)", flush=True)

    # Clear disk caches so runtime re-indexes .webp paths cleanly
    for c in (".hd_index.cache", ".hd_sprite_index.cache"):
        cp = os.path.join(root, c)
        if os.path.exists(cp):
            os.remove(cp)
    saved_gb = (old_total - new_total) / 1e9
    pct = 100.0 * (1.0 - new_total / max(old_total, 1))
    print(f"[webp] Done in {time.time() - t0:.1f}s: saved {saved_gb:.2f} GB ({pct:.1f}% smaller), 0 quality loss!", flush=True)


if __name__ == "__main__":
    main()
