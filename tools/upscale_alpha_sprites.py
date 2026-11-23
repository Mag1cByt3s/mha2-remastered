#!/usr/bin/env python3
"""
Re-upscale every IT3 frame that carries a semi-transparent tbl2 layer (Moorhuhn s/*,
cursors g/img/mc*, characters, glows) as a proper composited tbl1+tbl2 RGBA sprite.

Why: mha2.exe draws each such frame as an opaque tbl1 core PLUS a separate anti-aliased
tbl2 edge ring (eyes, feathers, outlines). The old pack only upscaled tbl1, so the runtime
stamped the blurry 1x tbl2 ring on top of the 4K core.

Method (keeps detail, no halos):
  1) Composite tbl1 + tbl2 into one RGBA frame at the exact t0 cell size.
  2) Dilate RGB into transparent pixels (no black/magenta fringe for the network).
  3) Upscale RGB and the alpha mask separately with the chosen sprite model (ultrasharp-4x).
  4) Recombine -> RGBA, write to hd_assets/sprites/<same rel name>.png.

Run inside:  nix-shell -p python3Packages.pillow python3Packages.numpy --run \
               "python3 tools/upscale_alpha_sprites.py"
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_assets import Mha2Archive, decode_it3_frame  # noqa: E402
from upscale_ai import find_upscaler_cmd, ensure_custom_model  # noqa: E402


def composite_frame(raw, palette, fi, t0_off, t0_cnt, t1_off, t1_cnt, t2_off, t2_cnt):
    cw = ch = 0
    if t0_off and fi < t0_cnt and t0_off + (fi + 1) * 17 <= len(raw):
        _, _, cw, ch = struct.unpack_from("<4I", raw, t0_off + fi * 17)
    layers = []
    for off, cnt in ((t1_off, t1_cnt), (t2_off, t2_cnt)):
        if off and fi < cnt:
            foff = struct.unpack_from("<I", raw, off + fi * 4)[0]
            if foff:
                dec = decode_it3_frame(raw, foff, palette, cw, ch)
                if dec:
                    layers.append(dec)
    if not layers:
        return None
    w = max(d[0] for d in layers)
    h = max(d[1] for d in layers)
    out = np.zeros((h, w, 4), dtype=np.uint8)
    for lw, lh, buf in layers:
        a = np.frombuffer(buf, dtype=np.uint8).reshape(lh, lw, 4)
        m = a[:, :, 3] > 0
        out[:lh, :lw][m] = a[m]
    return out


def dilate_rgb(rgba, iters=16):
    rgb = rgba[:, :, :3].astype(np.float32)
    mask = (rgba[:, :, 3] > 0).astype(np.float32)
    for _ in range(iters):
        if mask.all():
            break
        p_rgb = np.pad(rgb * mask[:, :, None], ((1, 1), (1, 1), (0, 0)), mode="edge")
        p_m = np.pad(mask, ((1, 1), (1, 1)), mode="edge")
        s_rgb = (p_rgb[:-2, :-2] + p_rgb[:-2, 1:-1] + p_rgb[:-2, 2:] +
                 p_rgb[1:-1, :-2] + p_rgb[1:-1, 2:] +
                 p_rgb[2:, :-2] + p_rgb[2:, 1:-1] + p_rgb[2:, 2:])
        s_m = (p_m[:-2, :-2] + p_m[:-2, 1:-1] + p_m[:-2, 2:] +
               p_m[1:-1, :-2] + p_m[1:-1, 2:] +
               p_m[2:, :-2] + p_m[2:, 1:-1] + p_m[2:, 2:])
        fill = (mask == 0) & (s_m > 0)
        if not fill.any():
            break
        rgb[fill] = s_rgb[fill] / s_m[fill, None]
        mask[fill] = 1.0
    return np.clip(rgb, 0, 255).astype(np.uint8)


def run_upscaler(cmd_prefix, in_dir, out_dir, model, model_dir, gpu, scale):
    os.makedirs(out_dir, exist_ok=True)
    cmd = cmd_prefix + ["-i", in_dir, "-o", out_dir, "-n", model, "-s", str(scale),
                        "-t", "256", "-f", "png", "-j", "1:2:2"]
    if model_dir:
        cmd += ["-m", model_dir]
    if gpu != "auto":
        cmd += ["-g", str(gpu)]
    try:
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=900)
    except subprocess.TimeoutExpired:
        print("[WARN] upscaler batch timed out; falling back to Lanczos for missing files", flush=True)


def lanczos_up(img, scale):
    return img.resize((img.width * scale, img.height * scale), Image.LANCZOS)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dat", default="game_files/mha2.dat")
    ap.add_argument("-o", "--out", default="hd_assets")
    ap.add_argument("--model", default="ultrasharp-4x")
    ap.add_argument("--model-dir", default="tools/models")
    ap.add_argument("--gpu", default="1")
    ap.add_argument("--scale", type=int, default=4)
    ap.add_argument("--batch", type=int, default=400)
    ap.add_argument("--filter", default="", help="Only paths containing this substring")
    ap.add_argument("--non-tbl2", action="store_true",
                    help="Process non-tbl2 IT3 sprites with 1-bit transparency (props, items, icons) for smooth 4K AA edges")
    ap.add_argument("--backup", default="work/sprites_tbl1_backup")
    args = ap.parse_args()

    cmd_prefix = find_upscaler_cmd()
    mdir = ensure_custom_model(args.model_dir, args.model)
    spr_root = os.path.join(args.out, "sprites")
    arc = Mha2Archive(args.dat)

    jobs = []  # (rel_png, rgba ndarray)
    for e in arc.entries:
        path = e["path"]
        if not path.endswith("._img") or (args.filter and args.filter not in path):
            continue
        low = path.lower()
        if "/fnt" in low or low.endswith("/objfont._img"):
            continue
        raw = arc.read_entry(e)
        if len(raw) < 0x50 or struct.unpack_from("<I", raw, 0)[0] != 1:
            continue
        pal_off, pal_cnt = struct.unpack_from("<II", raw, 0x28)
        palette = list(struct.unpack_from(f"<{pal_cnt}I", raw, pal_off)) \
            if (pal_off and pal_cnt and pal_off + pal_cnt * 4 <= len(raw)) else []
        t0_off, t0_cnt, t1_off, t1_cnt, t2_off, t2_cnt = struct.unpack_from("<6I", raw, 0x38)
        has_t2 = bool(t2_off and t2_cnt)
        if args.non_tbl2:
            if has_t2 or not (t1_off and t1_cnt):
                continue
        else:
            if not has_t2:
                continue
        n = max(t1_cnt, t2_cnt)
        for fi in range(n):
            rgba = composite_frame(raw, palette, fi, t0_off, t0_cnt, t1_off, t1_cnt, t2_off, t2_cnt)
            if rgba is None or rgba.shape[0] < 16 or rgba.shape[1] < 16:
                continue
            if args.non_tbl2 and (rgba[:, :, 3] == 255).all():
                continue  # 100% opaque rectangle — no silhouette edge to anti-alias
            suffix = "" if n == 1 else f"_f{fi:03d}"
            jobs.append((path[:-5] + suffix + ".png", rgba))
    arc.close()
    mode_lbl = "non-tbl2 1-bit-alpha props/items" if args.non_tbl2 else "composited tbl1+tbl2 frames"
    print(f"[ALPHA] {len(jobs)} {mode_lbl} to upscale with {args.model}", flush=True)

    pad = 4 if args.non_tbl2 else 0
    pad4 = pad * args.scale
    stage = tempfile.mkdtemp(prefix="mha2_alpha_")
    done = 0
    t_start = time.time()
    try:
        for b0 in range(0, len(jobs), args.batch):
            chunk = jobs[b0:b0 + args.batch]
            in_dir = os.path.join(stage, "in")
            out_dir = os.path.join(stage, "out")
            shutil.rmtree(in_dir, ignore_errors=True)
            shutil.rmtree(out_dir, ignore_errors=True)
            os.makedirs(in_dir)
            for i, (_, rgba) in enumerate(chunk):
                rgb = dilate_rgb(rgba)
                a = rgba[:, :, 3]
                if pad > 0:
                    rgb = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="edge")
                    a = np.pad(a, ((pad, pad), (pad, pad)), mode="constant", constant_values=0)
                Image.fromarray(rgb, "RGB").save(os.path.join(in_dir, f"{i:05d}_c.png"))
                Image.fromarray(np.stack([a, a, a], axis=-1), "RGB").save(os.path.join(in_dir, f"{i:05d}_a.png"))
            run_upscaler(cmd_prefix, in_dir, out_dir, args.model, mdir, args.gpu, args.scale)

            for i, (rel_png, rgba) in enumerate(chunk):
                h, w = rgba.shape[:2]
                tw, th = w * args.scale, h * args.scale
                ptw, pth = (w + 2 * pad) * args.scale, (h + 2 * pad) * args.scale
                cp = os.path.join(out_dir, f"{i:05d}_c.png")
                apth = os.path.join(out_dir, f"{i:05d}_a.png")
                c_img = Image.open(cp).convert("RGB") if os.path.exists(cp) else \
                    lanczos_up(Image.open(os.path.join(in_dir, f"{i:05d}_c.png")).convert("RGB"), args.scale)
                a_img = Image.open(apth).convert("L") if os.path.exists(apth) else \
                    lanczos_up(Image.open(os.path.join(in_dir, f"{i:05d}_a.png")).convert("L"), args.scale)
                if c_img.size != (ptw, pth):
                    c_img = c_img.resize((ptw, pth), Image.LANCZOS)
                if a_img.size != (ptw, pth):
                    a_img = a_img.resize((ptw, pth), Image.LANCZOS)
                if pad4 > 0:
                    c_img = c_img.crop((pad4, pad4, pad4 + tw, pad4 + th))
                    a_img = a_img.crop((pad4, pad4, pad4 + tw, pad4 + th))
                if args.non_tbl2:
                    a_img = a_img.filter(ImageFilter.GaussianBlur(radius=1.0))
                a4 = np.array(a_img)
                a4 = np.where(a4 <= 8, 0, np.where(a4 >= 248, 255, a4)).astype(np.uint8)
                out = np.dstack([np.array(c_img), a4])

                dst = os.path.join(spr_root, rel_png)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if args.backup and os.path.exists(dst):
                    bk = os.path.join(args.backup, rel_png)
                    if not os.path.exists(bk):
                        os.makedirs(os.path.dirname(bk), exist_ok=True)
                        shutil.copy2(dst, bk)
                Image.fromarray(out, "RGBA").save(dst)
                done += 1
            el = time.time() - t_start
            print(f"[ALPHA] {done}/{len(jobs)} done ({el:.0f}s, ~{el / done * (len(jobs) - done):.0f}s left)",
                  flush=True)
    finally:
        shutil.rmtree(stage, ignore_errors=True)

    cache = os.path.join(args.out, ".hd_sprite_index.cache")
    if os.path.exists(cache):
        os.remove(cache)
    print(f"[ALPHA] Installed {done} composited 4x RGBA sprites into '{spr_root}'.", flush=True)


if __name__ == "__main__":
    main()
