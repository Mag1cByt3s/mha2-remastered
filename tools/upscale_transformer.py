#!/usr/bin/env python3
"""
Moorhuhn Adventure 2 — Modern Faithful Transformer Super-Resolution (DAT / HAT / SPAN)
100% art-faithful (ZERO diffusion hallucination, ZERO style drift). Uses modern 2024+
Transformer/SPAN architectures (4xNomos8kDAT, Real_HAT_GAN_SRx4, 4x-ClearRealityV1)
trained to strip 2005 JPEG compression & staircases while keeping every original detail razor-sharp.
"""
import argparse
import io
import os
import struct
import sys
import time
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter
import torch
import spandrel
import spandrel_extra_arches

spandrel_extra_arches.install()

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_assets import Mha2Archive, decode_it3_frame
from upscale_alpha_sprites import composite_frame, dilate_rgb


def load_sr_model(model_path: str, device: str = "cuda"):
    desc = spandrel.ModelLoader().load_from_file(model_path)
    desc.to(device).eval()
    return desc


@torch.inference_mode()
def upscale_tiled(desc, img: Image.Image, tile: int = 256, overlap: int = 32, device: str = "cuda") -> Image.Image:
    """Seamless tiled 4x inference in FP32 (avoids attention overflow on DAT/HAT)."""
    arr = np.asarray(img.convert("RGB"), dtype=np.float32) / 255.0
    h, w, _ = arr.shape
    scale = desc.scale

    out = np.zeros((h * scale, w * scale, 3), dtype=np.float32)
    weight = np.zeros((h * scale, w * scale, 1), dtype=np.float32)

    step = tile - overlap
    ys = list(range(0, max(1, h - tile + 1), step))
    if ys[-1] + tile < h:
        ys.append(max(0, h - tile))
    xs = list(range(0, max(1, w - tile + 1), step))
    if xs[-1] + tile < w:
        xs.append(max(0, w - tile))

    for y0 in ys:
        y1 = min(h, y0 + tile)
        for x0 in xs:
            x1 = min(w, x0 + tile)
            patch = arr[y0:y1, x0:x1]
            ten = torch.from_numpy(patch).permute(2, 0, 1).unsqueeze(0).to(device)
            pred = desc(ten).squeeze(0).permute(1, 2, 0).clamp(0.0, 1.0).cpu().numpy()

            oh, ow, _ = pred.shape
            wy = np.ones(oh, dtype=np.float32)
            wx = np.ones(ow, dtype=np.float32)
            ov = overlap * scale
            if y0 > 0 and ov < oh:
                wy[:ov] = np.linspace(0.0, 1.0, ov, dtype=np.float32)
            if y1 < h and ov < oh:
                wy[-ov:] = np.linspace(1.0, 0.0, ov, dtype=np.float32)
            if x0 > 0 and ov < ow:
                wx[:ov] = np.linspace(0.0, 1.0, ov, dtype=np.float32)
            if x1 < w and ov < ow:
                wx[-ov:] = np.linspace(1.0, 0.0, ov, dtype=np.float32)
            mask = np.outer(wy, wx)[..., None]

            out[y0 * scale : y1 * scale, x0 * scale : x1 * scale] += pred * mask
            weight[y0 * scale : y1 * scale, x0 * scale : x1 * scale] += mask

    res = np.clip(out / np.maximum(weight, 1e-6), 0.0, 1.0)
    return Image.fromarray((res * 255.0 + 0.5).astype(np.uint8))


def extract_raw_bg(arc: Mha2Archive, entry_name: str) -> Image.Image:
    target = entry_name.lower()
    for e in arc.entries:
        name = e["path"] if isinstance(e, dict) else e[0]
        if name.lower().replace("\\", "/") == target:
            raw = arc.read_entry(e)
            img_type = struct.unpack_from("<I", raw, 0)[0]
            if img_type != 0:
                raise RuntimeError(f"{entry_name} is not a JPEG bg (type={img_type})")
            joff, jsz = struct.unpack_from("<II", raw, 0x40)
            jpg = raw[joff : joff + jsz]
            return Image.open(io.BytesIO(jpg)).convert("RGB")
    raise RuntimeError(f"Entry not found in mha2.dat: {entry_name}")


def draw_label(im: Image.Image, text: str, xy=(14, 12)):
    draw = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype("/run/current-system/sw/share/X11/fonts/DejaVuSans-Bold.ttf", 28)
    except Exception:
        font = ImageFont.load_default()
    bbox = draw.textbbox(xy, text, font=font)
    pad = 8
    draw.rectangle((bbox[0] - pad, bbox[1] - pad, bbox[2] + pad, bbox[3] + pad), fill=(15, 18, 28))
    draw.text(xy, text, fill=(255, 224, 120), font=font)


def make_4way_zoom_sheet(images_dict: dict, crops: list, out_path: str):
    """2x2 grid per crop showing 4 models side-by-side at native 1:1 4K pixels."""
    names = list(images_dict.keys())
    bw, bh = next(iter(images_dict.values())).size
    cw, ch = 640, 440
    gap = 6
    block_h = ch * 2 + gap
    sheet = Image.new("RGB", (cw * 2 + gap, (block_h + 14) * len(crops)), (20, 24, 34))

    for ci, (cname, (cx, cy)) in enumerate(crops):
        cx = max(0, min(cx, bw - cw))
        cy = max(0, min(cy, bh - ch))
        by = ci * (block_h + 14)
        for mi, mname in enumerate(names[:4]):
            im = images_dict[mname].crop((cx, cy, cx + cw, cy + ch))
            draw_label(im, f"{mname} | {cname}", (10, 8))
            gx = (mi % 2) * (cw + gap)
            gy = by + (mi // 2) * (ch + gap)
            sheet.paste(im, (gx, gy))
    sheet.save(out_path)


@torch.inference_mode()
def upscale_fast(desc, arr_u8: np.ndarray, device: str = "cuda") -> np.ndarray:
    """Fast single-pass GPU 4x upscale for small/medium sprites, tiled for large ones."""
    h, w = arr_u8.shape[:2]
    if h <= 384 and w <= 384:
        ten = torch.from_numpy(arr_u8.astype(np.float32) / 255.0).permute(2, 0, 1).unsqueeze(0).to(device)
        pred = desc(ten).squeeze(0).permute(1, 2, 0).clamp(0.0, 1.0).cpu().numpy()
        return (pred * 255.0 + 0.5).astype(np.uint8)
    return np.asarray(upscale_tiled(desc, Image.fromarray(arr_u8, "RGB"), tile=256, overlap=32, device=device))


def find_hd_asset(stem: str) -> str:
    for ext in (".webp", ".png"):
        p = stem + ext
        if os.path.exists(p):
            return p
    return stem + ".webp"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compare", action="store_true", help="Generate 4-way 1:1 faithful SR comparisons")
    ap.add_argument("--apply-backgrounds", metavar="MODEL_PATH", help="Batch-upscale all single-frame JPEG backgrounds/screens")
    ap.add_argument("--apply-jpeg-strips", metavar="MODEL_PATH",
                    help="Split & 4x-upscale all 50 multi-frame JPEG vertical strips into per-frame lossless WebPs in hd_assets/sprites/")
    ap.add_argument("--apply-sprites", metavar="MODEL_PATH", help="Batch-upscale ALL ~8,297 IT3 sprites (tbl2 + non-tbl2 + opaque)")
    ap.add_argument("--out-dir", default="work/sr_samples")
    args = ap.parse_args()

    arc = Mha2Archive("game_files/mha2.dat")
    device = "cuda" if torch.cuda.is_available() else "cpu"

    if args.compare:
        os.makedirs(args.out_dir, exist_ok=True)
        models = [
            ("4xNomos8kDAT (DAT Transformer)", "tools/models/sr/4xNomos8kDAT.safetensors"),
            ("Real_HAT_GAN_SRx4 (HAT Transformer)", "tools/models/sr/Real_HAT_GAN_SRx4.pth"),
            ("4x-ClearRealityV1 (SPAN)", "tools/models/sr/4x-ClearRealityV1.safetensors"),
        ]
        rooms = [
            ("room_001_xobjBg", "l/001/img/xobjbg._img", find_hd_asset("hd_assets/l/001/img/xobjBg"), [
                ("Chandelier & Window", (1280, 0)),
                ("Stone Wall & Curtain", (1950, 450)),
                ("Globe & Books", (1300, 1150)),
            ]),
            ("room_005_objBg", "l/005/img/objbg._img", find_hd_asset("hd_assets/l/005/img/objBg"), [
                ("Banner Crest", (1280, 120)),
                ("Red Carpet & Knight", (1250, 1150)),
                ("Cobweb & Wood Door", (2150, 560)),
            ]),
        ]

        # Extract raw 1x backgrounds
        raws = {}
        results = {}
        for tag, dat_path, cur_hd, _ in rooms:
            raw = extract_raw_bg(arc, dat_path)
            raws[tag] = raw
            print(f"[raw] {tag}: {raw.size} from {dat_path}", flush=True)
            results[tag] = {
                "CURRENT: 4x_NMKD-Siax (2021 GAN)": Image.open(cur_hd).convert("RGB"),
            }

        for mlabel, mpath in models:
            print(f"\n[load] {mlabel} ({mpath})", flush=True)
            desc = load_sr_model(mpath, device)
            for tag, _, _, _ in rooms:
                t0 = time.time()
                up = upscale_tiled(desc, raws[tag], tile=256, overlap=32, device=device)
                print(f"  {tag} -> {up.size} in {time.time() - t0:.2f}s", flush=True)
                results[tag][mlabel] = up
                short = mlabel.split()[0].replace("-", "_")
                up.save(os.path.join(args.out_dir, f"{tag}_{short}.png"), compress_level=3)
            del desc
            torch.cuda.empty_cache()

        for tag, _, _, crops in rooms:
            out_sheet = os.path.join(args.out_dir, f"{tag}_4way_zoom_1to1.png")
            make_4way_zoom_sheet(results[tag], crops, out_sheet)
            print(f"[saved] {out_sheet}", flush=True)

    elif args.apply_backgrounds:
        desc = load_sr_model(args.apply_backgrounds, device)
        count = 0
        t_all = time.time()
        for e in arc.entries:
            name = e["path"] if isinstance(e, dict) else e[0]
            norm = name.replace("\\", "/")
            if not norm.lower().endswith("._img"):
                continue
            raw = arc.read_entry(e)
            if len(raw) < 0x50:
                continue
            img_type = struct.unpack_from("<I", raw, 0)[0]
            if img_type != 0:
                continue
            nf = struct.unpack_from("<I", raw, 4)[0]
            if nf > 1:
                continue
            joff, jsz = struct.unpack_from("<II", raw, 0x40)
            if joff + jsz > len(raw) or jsz <= 4:
                continue
            im = Image.open(io.BytesIO(raw[joff : joff + jsz])).convert("RGB")
            up = upscale_tiled(desc, im, tile=256, overlap=32, device=device)
            stem_rel = norm[:-5]
            out_path = os.path.join("hd_assets", stem_rel + ".webp")
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            up.save(out_path, "WEBP", lossless=True, exact=True, method=4)
            stale_png = os.path.join("hd_assets", stem_rel + ".png")
            if os.path.exists(stale_png):
                os.remove(stale_png)
            count += 1
            if count % 10 == 0:
                print(f"  [{count}] {out_path} ({up.size}) elapsed {time.time() - t_all:.1f}s", flush=True)
        arc.close()
        bg_cache = os.path.join("hd_assets", ".hd_index.cache")
        if os.path.exists(bg_cache):
            os.remove(bg_cache)
        print(f"[done] Remastered {count} backgrounds in {time.time() - t_all:.1f}s (index cache cleared)", flush=True)

    elif args.apply_jpeg_strips:
        desc = load_sr_model(args.apply_jpeg_strips, device)
        scale = desc.scale
        spr_root = os.path.join("hd_assets", "sprites")
        jobs = []
        removed_strays = 0
        removed_unsplit = 0
        for e in arc.entries:
            name = e["path"] if isinstance(e, dict) else e[0]
            norm = name.replace("\\", "/")
            if not norm.lower().endswith("._img"):
                continue
            raw = arc.read_entry(e)
            if len(raw) < 0x50 or struct.unpack_from("<I", raw, 0)[0] != 0:
                continue
            nf, w, h = struct.unpack_from("<3I", raw, 4)
            joff, jsz = struct.unpack_from("<II", raw, 0x40)
            if nf <= 1 or joff + jsz > len(raw) or jsz <= 4:
                continue

            stem = norm[:-5]
            for ext in (".webp", ".png"):
                stray_p = os.path.join("hd_assets", stem + ext)
                if os.path.exists(stray_p):
                    os.remove(stray_p)
                    removed_strays += 1

            assert (h % nf) == 0, f"{norm}: height {h} not divisible by nf={nf}"
            fh = h // nf
            if w < 16 or fh < 16:
                continue

            im = Image.open(io.BytesIO(raw[joff : joff + jsz])).convert("RGB")
            arr = np.asarray(im, dtype=np.uint8)
            assert arr.shape[0] == h and arr.shape[1] == w, (
                f"{norm}: decoded shape {arr.shape} != expected ({h}, {w})"
            )

            for ext in (".webp", ".png"):
                unsplit_p = os.path.join(spr_root, stem + ext)
                if os.path.exists(unsplit_p):
                    os.remove(unsplit_p)
                    removed_unsplit += 1

            jobs.append((stem, nf, w, fh, arr))
        arc.close()

        total_frames = sum(nf for _, nf, _, _, _ in jobs)
        print(
            f"[jpeg-strips] Splitting & 4x-remastering {len(jobs)} strips ({total_frames} frames) "
            f"with {args.apply_jpeg_strips} (removed {removed_strays} root strays, {removed_unsplit} unsplit sprites)...",
            flush=True,
        )
        t_all = time.time()
        pad = 16
        done_frames = 0
        for stem, nf, w, fh, arr in jobs:
            for fi in range(nf):
                frame = arr[fi * fh : (fi + 1) * fh, 0 : w, :3]
                padded = np.pad(frame, ((pad, pad), (pad, pad), (0, 0)), mode="edge")
                c4_pad = upscale_fast(desc, padded, device)
                c4 = c4_pad[pad * scale : pad * scale + fh * scale, pad * scale : pad * scale + w * scale]
                dst = os.path.join(spr_root, f"{stem}_f{fi:03d}.webp")
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                Image.fromarray(c4, "RGB").save(dst, "WEBP", lossless=True, exact=True, method=4)
                done_frames += 1
            if done_frames % 50 == 0 or done_frames == total_frames:
                el = time.time() - t_all
                print(f"  [jpeg-strips] {done_frames}/{total_frames} frames ({el:.1f}s elapsed)", flush=True)

        for cname in (".hd_sprite_index.cache", ".hd_index.cache"):
            cpath = os.path.join("hd_assets", cname)
            if os.path.exists(cpath):
                os.remove(cpath)
        print(f"[done] Remastered {done_frames} frames across {len(jobs)} JPEG strips in {time.time() - t_all:.1f}s.", flush=True)

    elif args.apply_sprites:
        desc = load_sr_model(args.apply_sprites, device)
        scale = desc.scale
        spr_root = os.path.join("hd_assets", "sprites")
        jobs = []  # (rel_stem, rgba, has_t2, is_opaque)
        for e in arc.entries:
            path = e["path"] if isinstance(e, dict) else e[0]
            if not path.endswith("._img"):
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
            n = max(t1_cnt, t2_cnt)
            for fi in range(n):
                rgba = composite_frame(raw, palette, fi, t0_off, t0_cnt, t1_off, t1_cnt, t2_off, t2_cnt)
                if rgba is None or rgba.shape[0] < 16 or rgba.shape[1] < 16:
                    continue
                is_opaque = bool((rgba[:, :, 3] == 255).all())
                suffix = "" if n == 1 else f"_f{fi:03d}"
                jobs.append((path[:-5] + suffix, rgba, has_t2, is_opaque))
        arc.close()
        print(f"[sprites] Upscaling ALL {len(jobs)} IT3 sprites with {args.apply_sprites}...", flush=True)
        t_all = time.time()
        for idx, (rel_stem, rgba, has_t2, is_opaque) in enumerate(jobs, 1):
            h, w = rgba.shape[:2]
            tw, th = w * scale, h * scale
            if is_opaque:
                c4 = upscale_fast(desc, rgba[:, :, :3], device)
                a4 = np.full((th, tw), 255, dtype=np.uint8)
            else:
                rgb = dilate_rgb(rgba)
                a = rgba[:, :, 3]
                pad = 0 if has_t2 else 4
                pad4 = pad * scale
                if pad > 0:
                    rgb = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="edge")
                    a = np.pad(a, ((pad, pad), (pad, pad)), mode="constant", constant_values=0)
                c4 = upscale_fast(desc, rgb, device)
                a_rgb = np.stack([a, a, a], axis=-1)
                a4 = upscale_fast(desc, a_rgb, device)[:, :, 0]
                if pad4 > 0:
                    c4 = c4[pad4 : pad4 + th, pad4 : pad4 + tw]
                    a4 = a4[pad4 : pad4 + th, pad4 : pad4 + tw]
                if not has_t2:
                    a4 = np.asarray(Image.fromarray(a4, "L").filter(ImageFilter.GaussianBlur(radius=1.0)))
                a4 = np.where(a4 <= 8, 0, np.where(a4 >= 248, 255, a4)).astype(np.uint8)
            out = np.dstack([c4, a4])
            dst = os.path.join(spr_root, rel_stem + ".webp")
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            Image.fromarray(out, "RGBA").save(dst, "WEBP", lossless=True, exact=True, method=4)
            stale_png = os.path.join(spr_root, rel_stem + ".png")
            if os.path.exists(stale_png):
                os.remove(stale_png)
            if idx % 500 == 0 or idx == len(jobs):
                el = time.time() - t_all
                eta = el / idx * (len(jobs) - idx)
                print(f"  [sprites] {idx}/{len(jobs)} ({el:.1f}s elapsed, ~{eta:.0f}s left)", flush=True)

        cache = os.path.join("hd_assets", ".hd_sprite_index.cache")
        if os.path.exists(cache):
            os.remove(cache)
        print(f"[done] Remastered ALL {len(jobs)} sprites in {time.time() - t_all:.1f}s (HDS5 cache cleared).", flush=True)


if __name__ == "__main__":
    main()
