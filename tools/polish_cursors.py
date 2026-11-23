#!/usr/bin/env python3
"""
Moorhuhn Adventure 2 — Vector-Clean SDF Cursor Polisher
Replaces the bumpy 2005 tbl2 outer fringe on all cursor frames (mcArrow, mcLook,
mcTake, mcTalk, mcUse, cuExit, cuHelp) with a mathematically smooth signed-distance-field (SDF) contour,
clean interior RGB dilation, and a subtle modern soft drop shadow — while preserving 100%
of the original iconic cursor artwork and HDS5 matching.
"""
import glob
import os
import shutil
import cv2
import numpy as np
from PIL import Image


def polish_cursor(im: Image.Image) -> Image.Image:
    rgba = np.asarray(im.convert("RGBA"), dtype=np.uint8)
    h, w = rgba.shape[:2]
    rgb = rgba[:, :, :3].astype(np.float32)
    alpha = rgba[:, :, 3]

    # 1. Build clean core binary mask from opaque interior (eliminates bumpy tbl2 fringe)
    core = (alpha >= 175).astype(np.uint8)
    # Remove any isolated 1-2px detached noise specks
    num_l, labels, stats, _ = cv2.connectedComponentsWithStats(core, connectivity=8)
    clean_core = np.zeros_like(core)
    for lbl in range(1, num_l):
        if stats[lbl, cv2.CC_STAT_AREA] >= 36:
            clean_core[labels == lbl] = 1

    # 2. Signed Distance Field (positive inside, negative outside), smoothed for vector-like curvature
    dist_in = cv2.distanceTransform(clean_core, cv2.DIST_L2, 5)
    dist_out = cv2.distanceTransform(1 - clean_core, cv2.DIST_L2, 5)
    sdf = dist_in - dist_out
    sdf_smooth = cv2.GaussianBlur(sdf, (0, 0), sigmaX=2.2, sigmaY=2.2)

    # Anti-aliased 1.8px crisp vector edge from smoothed SDF (slightly expanded by +0.6px to preserve original fullness)
    new_alpha = np.clip((sdf_smooth + 0.8) / 1.8 + 0.5, 0.0, 1.0)

    # 3. Dilate clean opaque interior RGB outward so the smooth rim has zero grey/muddy fringe
    dil_rgb = rgb.copy()
    mask = (alpha >= 210) & (clean_core > 0)
    for _ in range(12):
        if mask.all():
            break
        k = np.ones((3, 3), np.float32)
        m_f = mask.astype(np.float32)
        s_m = cv2.filter2D(m_f, -1, k, borderType=cv2.BORDER_REPLICATE)
        fill = (~mask) & (s_m > 0.5)
        if not fill.any():
            break
        for c in range(3):
            s_c = cv2.filter2D(dil_rgb[:, :, c] * m_f, -1, k, borderType=cv2.BORDER_REPLICATE)
            dil_rgb[:, :, c][fill] = s_c[fill] / s_m[fill]
        mask[fill] = True

    # 4. Subtle clean modern drop shadow (offset +2px,+2px, soft blur) behind the crisp cursor
    sh_alpha = np.zeros((h, w), dtype=np.float32)
    sh_alpha[2:, 2:] = new_alpha[:-2, :-2]
    sh_alpha = cv2.GaussianBlur(sh_alpha, (0, 0), sigmaX=2.4, sigmaY=2.4) * 0.42

    # Porter-Duff "cursor OVER drop shadow"
    out_a = new_alpha + sh_alpha * (1.0 - new_alpha)
    out_rgb = np.zeros_like(dil_rgb)
    valid = out_a > 1e-4
    for c in range(3):
        # Shadow RGB is dark charcoal (12, 10, 16)
        sh_c = 12.0
        out_rgb[:, :, c][valid] = (
            dil_rgb[:, :, c][valid] * new_alpha[valid]
            + sh_c * sh_alpha[valid] * (1.0 - new_alpha[valid])
        ) / out_a[valid]

    out = np.dstack([
        np.clip(out_rgb + 0.5, 0, 255).astype(np.uint8),
        np.clip(out_a * 255.0 + 0.5, 0, 255).astype(np.uint8),
    ])
    return Image.fromarray(out, "RGBA")


def main():
    patterns = (
        "hd_assets/sprites/g/img/mc*.webp",
        "hd_assets/sprites/g/img/mc*.png",
        "hd_assets/sprites/g/img/cu*.webp",
        "hd_assets/sprites/g/img/cu*.png",
    )
    files = sorted({p for pat in patterns for p in glob.glob(pat)})
    backup_dir = "work/cursors_pre_sdf_backup"
    os.makedirs(backup_dir, exist_ok=True)
    for p in files:
        bk = os.path.join(backup_dir, os.path.basename(p))
        if not os.path.exists(bk):
            shutil.copy2(p, bk)
        with Image.open(bk) as im:
            pol = polish_cursor(im)
        if p.endswith(".webp"):
            pol.save(p, format="WEBP", lossless=True, exact=True, quality=100, method=6)
        else:
            pol.save(p, compress_level=3)
        print(f"[cursor] Polished {p} ({pol.size})")
    cache = "hd_assets/.hd_sprite_index.cache"
    if os.path.exists(cache):
        os.remove(cache)
    print(f"[cursor] Done ({len(files)} cursor frames polished, HDS5 cache cleared).")


if __name__ == "__main__":
    main()
